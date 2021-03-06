#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include "response.h"
#include "metadata.h"
#include "main.h"
#include "conn.h"
#include "util.h"
#include "error_map.h"
#include "cJSON/cJSON.h"

typedef void*(*parse_partition_func)(void*, int);

struct buffer *wait_response(int cfd) {
    int rbytes = 0, rc, r, remain, resp_size;
    struct buffer *response;

    rc = wait_socket_data(cfd, 3000, CR_READ);
    if (rc <= 0) { // timeout or error
       logger(DEBUG, "wait response error, as %s!", strerror(errno));
        return NULL;
    }

    response = alloc_buffer(128);
    remain = get_buffer_cap(response);
    TIME_START();
    while (rbytes < 4) {
        r = read(cfd, get_buffer_data(response) + rbytes, remain);
        if (r <= 0) {
            if (r == -1 &&
                   (errno == EAGAIN || errno == EINTR || errno == EWOULDBLOCK)) continue;
            if (r == 0) close(cfd);
            logger(DEBUG, "wait response error, as %s!", strerror(errno));
            goto err_cleanup;
        }
        rbytes += r;
        remain -= r;
        incr_buffer_used(response, r);
    }
    resp_size = read_int32_buffer(response) + 4; 
    need_expand(response, resp_size);
    while (rbytes < resp_size) {
        r = read(cfd, get_buffer_data(response) + rbytes, resp_size - rbytes);
        if (r <= 0) {
            if (r == -1 &&
                   (errno == EAGAIN || errno == EINTR || errno == EWOULDBLOCK)) continue;
            if (r == 0) close(cfd);
            logger(DEBUG, "wait response error, as %s!", strerror(errno));
            goto err_cleanup;
        }
        rbytes += r;
        incr_buffer_used(response, r);
    }
    TIME_END();
    logger(DEBUG, "Total time cost %lldus in wait response", TIME_COST());
    return response;

err_cleanup:
    dealloc_buffer(response);
    return NULL;
}

static struct messageset* alloc_messageset(int cap) {
    struct messageset *msg_set;

    if (cap < 4) cap = 4;
    msg_set = malloc(sizeof(*msg_set));
    msg_set->cap = cap;
    msg_set->used = 0;
    msg_set->msgs = malloc(cap * sizeof(struct message)); 
    return msg_set;
}

static void dealloc_messageset(struct messageset *msg_set) {
    int i;
    
    for (i = 0; i < msg_set->used; i++) {
        if (msg_set->msgs[i].key) free(msg_set->msgs[i].key);
        if (msg_set->msgs[i].value) free(msg_set->msgs[i].value);
    }
    free(msg_set->msgs);
    free(msg_set);
}

static int add_message(struct messageset *msg_set, char *key, int key_size,
        char *value, int value_size, int64_t offset) {
    struct message *msg;
    if (!msg_set) return 0;

    if (msg_set->used == msg_set->cap) {
        msg_set->cap *= 2;
        msg_set->msgs = realloc(msg_set->msgs, msg_set->cap * sizeof(struct message));
    }
    msg = &msg_set->msgs[msg_set->used++];
    msg->key = key;
    msg->key_size = key_size;
    msg->value = value;
    msg->value_size = value_size;
    msg->offset = offset;
    return 1;
}

static struct messageset *parse_message_set(struct buffer *resp_buf) {
    int size, key_size, value_size;
    long offset;
    char *key = NULL, *value = NULL;
    struct messageset *msg_set;

    msg_set = alloc_messageset(4);
    while(!is_buffer_eof(resp_buf)) {
        if (get_buffer_unread(resp_buf) < MSG_OVERHEAD) {
            skip_buffer_bytes(resp_buf, get_buffer_unread(resp_buf));
            break;
        }
        offset = read_int64_buffer(resp_buf);
        size = read_int32_buffer(resp_buf); // message size
        if (get_buffer_unread(resp_buf) < size) {
            skip_buffer_bytes(resp_buf, get_buffer_unread(resp_buf));
            break;
        }
        skip_buffer_bytes(resp_buf, 4 + 1 + 1); //skip crc + magic + attr
        key_size = read_int32_buffer(resp_buf); // key size
        if (key_size > 0) {
            key = malloc(key_size + 1);
            read_raw_string_buffer(resp_buf, key, key_size);
            key[key_size] = '\0';
        }
        value_size = read_int32_buffer(resp_buf); // value size
        if (value_size > 0) {
            value = malloc(value_size + 1);
            read_raw_string_buffer(resp_buf, value, value_size);
            value[value_size] = '\0';
        }
        // key/value should by freed by messageset
        add_message(msg_set, key, key_size, value, value_size, offset);
    }
    return msg_set;
}

static struct response *alloc_response(int topic_count) {
    struct response *r;

    r = malloc(sizeof(*r) + topic_count * sizeof(struct topic_info));
    r->topic_count = topic_count;
    return r;
}

void dealloc_response(struct response *r, int type) {
    int i, j;
    struct topic_info *t_info;
    struct fetch_part_info *p_info;

    if (!r) return;

    for (i = 0; i < r->topic_count; i++) {
        t_info = &r->t_infos[i];
        if (t_info->name) free(t_info->name);
        if (!t_info->p_infos) break;

        for (j = 0; j < t_info->part_count; j++) {
            if (type == FETCH_KEY) {
                p_info = &t_info->p_infos[j];
                dealloc_messageset(p_info->msg_set);
            } else if (type == OFFSET_KEY) {
                struct offsets_part_info *p_info;
                p_info = &t_info->p_infos[j];
                free(p_info->offsets);
            }
        }
        free(t_info->p_infos);
    }

    free(r);
}


void* parse_offsets_part_infos(void *resp_buf, int part_count) {
    int i, j, offset_count;
    struct offsets_part_info *p_infos, *p_info;

    p_infos = malloc(part_count * sizeof(struct offsets_part_info));
    for (i = 0; i < part_count; i++) {
        p_info = &p_infos[i]; 
        p_info->part_id = read_int32_buffer(resp_buf); 
        p_info->err_code = read_int16_buffer(resp_buf); 
        offset_count = read_int32_buffer(resp_buf); 
        p_info->offset_count = offset_count;
        p_info->offsets = malloc(offset_count * sizeof(int64_t));
        for (j = 0; j < offset_count; j++) {
            p_info->offsets[j] = read_int64_buffer(resp_buf);
        }
    }
    return p_infos;
}

void* parse_produce_part_infos(void* resp_buf, int part_count) {
    int i;
    struct produce_part_info *p_infos, *p_info;

    resp_buf = (struct buffer *) resp_buf;
    p_infos = malloc(part_count * sizeof(struct produce_part_info));
    for (i = 0; i < part_count; i++) {
        p_info = &p_infos[i];
        p_info->part_id = read_int32_buffer(resp_buf); 
        p_info->err_code = read_int16_buffer(resp_buf); 
        p_info->offset = read_int64_buffer(resp_buf); 
    }
    return p_infos;
}

void *parse_fetch_part_infos(void *resp_buf, int part_count) {
    int i;
    struct fetch_part_info *p_infos, *p_info;

    resp_buf = (struct buffer *) resp_buf;
    p_infos = malloc(part_count * sizeof(struct fetch_part_info));
    for (i = 0; i < part_count; i++) {
        p_info = &p_infos[i];
        p_info->part_id = read_int32_buffer(resp_buf); 
        p_info->err_code = read_int16_buffer(resp_buf); 
        p_info->hw = read_int64_buffer(resp_buf);
        p_info->total_bytes = read_int32_buffer(resp_buf);
        p_info->msg_set = parse_message_set(resp_buf);
    }
    return p_infos;
}

struct response *parse_response(struct buffer *resp_buf, int type) {
    int i, topic_count;
    struct response *r;
    parse_partition_func func;

    read_int32_buffer(resp_buf); // corelation id
    topic_count = read_int32_buffer(resp_buf);
    if (topic_count <= 0) return NULL;

    r = alloc_response(topic_count);
    for (i = 0; i < topic_count; i++) {
        r->t_infos[i].name = read_short_string_buffer(resp_buf);
        r->t_infos[i].part_count = read_int32_buffer(resp_buf);
        if (r->t_infos[i].part_count <= 0) {
            r->t_infos[i].p_infos = NULL;
            return r;
        }
        switch(type) {
            case PRODUCE_KEY:
                func = parse_produce_part_infos;
                break;
             case OFFSET_KEY:
                func = parse_offsets_part_infos;
                break;
             case FETCH_KEY:
                func = parse_fetch_part_infos;
                break;
        }
        r->t_infos[i].p_infos = func(resp_buf, r->t_infos[i].part_count);
    }
    return r;
}

void dump_offsets_response(struct response *r) {
    int i, j, k;
    struct topic_info *t_info;
    struct offsets_part_info *p_info;

    if (!r) {
        logger(INFO, "fetch offset failed.");
        return;
    }

    for (i = 0; i < r->topic_count; i++) {
        t_info = &r->t_infos[i];
        printf("{ topic: %s, partitions: \n\t[\n", t_info->name);
        for (j = 0; j < t_info->part_count; j++) {
            p_info = &r->t_infos[i].p_infos[j]; 
            printf("\t\t{ part_id:%d, err_code:%d, offsets: [", 
                    p_info->part_id, p_info->err_code);
            for (k = 0; k < p_info->offset_count; k++) {
                if(k == p_info->offset_count -1) {
                    printf("%lld", p_info->offsets[k]);
                } else {
                    printf("%lld, ", p_info->offsets[k]);
                }
            }
            printf("] }\n");
        }
        printf("\t]\n}\n");
    }
}

void dump_produce_response(struct response *r) {
    int i, j;
    struct produce_part_info *p_info;

    if (!r) {
        logger(INFO, "produce failed.");
        return;
    }
    printf("[\n");
    for (i = 0; i < r->topic_count; i++) {
        printf("\t{ name :%s, partitions: [\n", r->t_infos[i].name);
        for (j = 0; j <  r->t_infos[i].part_count; j++) {
            p_info =  &r->t_infos[i].p_infos[j];
            printf("\t\t{part_id: %d, err_code: %d, offset: %lld},\n",
              p_info->part_id, p_info->err_code, p_info->offset);
        }
        printf("\t\t]\n\t}\n");
    }
    printf("]\n");
}

void dump_fetch_response(struct response *r) {
    int i, j, k;
    struct message *msg;
    struct fetch_part_info *p_info;

    if (!r) {
        logger(INFO, "fetch message failed.");
        return;
    }
    printf("[\n");
    for (i = 0; i < r->topic_count; i++) {
        printf("\t{ name :%s, partitions: [\n", r->t_infos[i].name);
        for (j = 0; j <  r->t_infos[i].part_count; j++) {
            p_info =  &r->t_infos[i].p_infos[j];
            printf("\t\t{part_id: %d, err_code: %d, highwater: %lld},\n",
              p_info->part_id, p_info->err_code, p_info->hw);
            for (k = 0; k < p_info->msg_set->used; k++) {
                msg = &p_info->msg_set->msgs[k];
                printf("\t\t\t{offset %lld, key: %s, key_size: %d, value: %s, value_size: %d}\n",
                   msg->offset, msg->key, msg->key_size, msg->value, msg->value_size);
            }
        }
        printf("\t\t]\n\t}\n");
    }
    printf("]\n");
}

cJSON *parse_broker_list(struct buffer *resp) {
    int i, id, port, broker_count;
    char *host;
    cJSON *brokers, *broker_obj;

    broker_count = read_int32_buffer(resp);
    brokers = cJSON_CreateArray();
    for(i = 0; i < broker_count; i++) {
        broker_obj = cJSON_CreateObject();
        id = read_int32_buffer(resp);
        host = read_short_string_buffer(resp);
        port = read_int32_buffer(resp);
        cJSON_AddNumberToObject(broker_obj, "id", id);
        cJSON_AddStringToObject(broker_obj, "host", host);
        cJSON_AddNumberToObject(broker_obj, "port", port);
        cJSON_AddItemToArray(brokers, broker_obj);
        free(host);
    }
    return brokers;
}

cJSON *parse_topic_metadata(struct buffer *resp) {
    int i, j, err_code, part_count, part_id, leader_id;
    int replica_count, *replicas, isr_count, *isr;
    char *topic;
    cJSON *topic_obj, *part_obj, *parts;

    topic_obj = cJSON_CreateObject();
    err_code = read_int16_buffer(resp);
    topic = read_short_string_buffer(resp);
    part_count = read_int32_buffer(resp);
    cJSON_AddNumberToObject(topic_obj, "err_code", err_code);
    if (err_code > 0) {
        cJSON_AddStringToObject(topic_obj, "err_msg", err_map[err_code]);
    }
    cJSON_AddStringToObject(topic_obj, "name", topic);

    parts = cJSON_CreateArray();
    cJSON_AddItemToObject(topic_obj, "partitions",parts);
    if (part_count == 0) return topic_obj;

    for(i = 0; i < part_count; i++) {
        part_obj = cJSON_CreateObject();
        err_code = read_int16_buffer(resp);
        part_id = read_int32_buffer(resp);
        leader_id = read_int32_buffer(resp);
        cJSON_AddNumberToObject(part_obj, "err_code", err_code);
        if (err_code > 0) {
            cJSON_AddStringToObject(topic_obj, "err_msg", err_map[err_code]);
        }
        cJSON_AddNumberToObject(part_obj, "part_id", part_id);
        cJSON_AddNumberToObject(part_obj, "leader_id", leader_id);
        replica_count = read_int32_buffer(resp);
        replicas = malloc(replica_count * sizeof(int));
        for (j = 0; j < replica_count; j++) {
            replicas[j] = read_int32_buffer(resp); 
        }
        cJSON_AddItemToObject(part_obj, "replicas", cJSON_CreateIntArray(replicas, replica_count));
        isr_count = read_int32_buffer(resp);
        isr = malloc(isr_count * sizeof(int));
        for (j = 0; j < isr_count; j++) {
            isr[j] = read_int32_buffer(resp); 
        }
        cJSON_AddItemToObject(part_obj, "isr", cJSON_CreateIntArray(replicas, replica_count));
        cJSON_AddItemToArray(parts, part_obj);
        free(replicas);
        free(isr);
    }

    free(topic);
    return topic_obj;
}

void dealloc_metadata_response(struct metadata_response *r) {
    if (!r) return;
    int i;
    
    for (i = 0; i < r->broker_count; i++) {
        if (r->b_metas[i].host) {
            free(r->b_metas[i].host);
        }
    }
    for (i = 0; i < r->topic_count; i++) {
        dealloc_topic_metadata(r->t_metas[i]);
    }
    free(r->b_metas);
    free(r->t_metas);
    free(r);
}

struct metadata_response *parse_metadata_response(struct buffer *resp_buf) {
    int i, j, k, old_pos, broker_count, topic_count, part_count, err_code;
    char *topic;
    struct broker_metadata *b_metas;
    struct topic_metadata *t_meta;
    struct partition_metadata* p_meta;
    struct metadata_response *r;

    if (!resp_buf) return NULL;

    old_pos = get_buffer_pos(resp_buf);
    r = malloc(sizeof(*r));
    if (!r) return NULL;
    read_int32_buffer(resp_buf); // ignore correlation id
    broker_count = read_int32_buffer(resp_buf);
    b_metas = malloc(broker_count * sizeof(struct broker_metadata)); 
    for(i = 0; i < broker_count; i++) {
        b_metas[i].id = read_int32_buffer(resp_buf);
        b_metas[i].host = read_short_string_buffer(resp_buf);
        b_metas[i].port = read_int32_buffer(resp_buf);
    }
    r->broker_count = broker_count;
    r->b_metas = b_metas;

    topic_count = read_int32_buffer(resp_buf);
    r->topic_count = topic_count;
    r->t_metas = calloc(topic_count, sizeof(void*));
    if (!r->t_metas) return NULL;
    for(i = 0; i < topic_count; i++) {
        err_code = read_int16_buffer(resp_buf);
        topic = read_short_string_buffer(resp_buf);
        part_count = read_int32_buffer(resp_buf);
        if (part_count == 0) {
            free(topic);
            continue;
        }
        t_meta = alloc_topic_metadata(part_count);
        t_meta->topic = strdup(topic);
        for(j = 0; j < part_count; j++) {
            p_meta = alloc_partition_metadata(); 
            t_meta->part_metas[j] = p_meta;
            err_code = read_int16_buffer(resp_buf); 
            p_meta->err_code = err_code;
            p_meta->part_id = read_int32_buffer(resp_buf);
            p_meta->leader_id = read_int32_buffer(resp_buf);
            p_meta->replica_count = read_int32_buffer(resp_buf);
            p_meta->replicas = malloc(p_meta->replica_count * sizeof(int));
            for (k = 0; k < p_meta->replica_count; k++) {
                p_meta->replicas[k] = read_int32_buffer(resp_buf);
            }
            p_meta->isr_count = read_int32_buffer(resp_buf);
            p_meta->isr = malloc( p_meta->isr_count * sizeof(int));
            for (k = 0; k < p_meta->isr_count; k++) {
                p_meta->isr[k] = read_int32_buffer(resp_buf);
            }
        }
        r->t_metas[i] = t_meta;
        free(topic);
    }

    reset_buffer_pos(resp_buf, old_pos);
    return r;
}

static void dump_topic_metadata(struct topic_metadata *t_meta) {
    int i, j;
    if (!t_meta) return;

    struct partition_metadata *p_meta;

    printf("{ topic = %s, partitions = %d, info = [\n", t_meta->topic, t_meta->partitions);
    for ( i = 0; i < t_meta->partitions; i++) {
        p_meta = t_meta->part_metas[i];
        printf("[ part_id = %d, leader_id = %d, replicas = [", p_meta->part_id, p_meta->leader_id);
        for (j = 0; j < p_meta->replica_count; j++) {
            if (j != p_meta->replica_count - 1) {
                printf("%d,", p_meta->replicas[j]);
            } else {
                printf("%d", p_meta->replicas[j]);
            }
        }
        printf("], isr = ["); 
        for(j = 0; j < p_meta->isr_count; j++)  {
            if (j != p_meta->isr_count - 1) {
                printf("%d,", p_meta->isr[j]);
            } else {
                printf("%d", p_meta->isr[j]);
            }
        }
        printf("]\n");
    }
    printf("]}\n");
}

void dump_metadata_response(struct metadata_response *r) {
    int i;

    if (!r) {
        logger(INFO, "dump metadata failed.");
        return;
    }
    for (i = 0; i < r->topic_count; i++) {
        dump_topic_metadata(r->t_metas[i]);
    }
}
