#include "password.h"
#include "pairs.h"
#include "protocol.h"
#include <xcopy.h>
#include <tcpcopy.h>

#define COM_STMT_PREPARE 22
#define COM_STMT_EXECUTE 23
#define COM_QUERY 3
#define MAX_SP_SIZE 256
#define MAX_USER_INFO 4096
#define ENCRYPT_LEN 16
#define SEED_323_LENGTH  8

typedef struct {
    time_t   last_refresh_time;
    uint32_t seq_after_ps;
    uint32_t sec_auth_checked:1;
    uint32_t sec_auth_not_yet_done:1;
    uint32_t first_auth_sent:1;
    uint32_t auth_packet_already_added:1;
    uint32_t update_auth_table_item_switch:4;
    char     scramble[SCRAMBLE_LENGTH + 1];
    char     seed323[SEED_323_LENGTH + 1];
    char     password[MAX_PASSWORD_LEN];
    char     user[MAX_USER_LEN];
    char     map_user[MAX_USER_LEN];
} tc_mysql_session;


typedef struct {
    link_list *list;
    int tot_cont_len;
} mysql_table_item_t;


typedef struct {
    tc_pool_t      *fir_auth_pool;
    tc_pool_t      *sec_auth_pool;
    tc_pool_t      *ps_pool;
    hash_table     *fir_auth_table;
    hash_table     *sec_auth_table;
    hash_table     *ps_table;
} tc_mysql_ctx_t;

/* TODO allocate it on heap */
static tc_mysql_ctx_t ctx;


static int 
init_mysql_module()
{
    tc_pool_t *pool;

    pool = tc_create_pool(TC_PLUGIN_POOL_SIZE, TC_PLUGIN_POOL_SUB_SIZE, 0);
    if (pool) {
#if (TC_DETECT_MEMORY)
        pool->d.is_traced = 1;
#endif
        ctx.fir_auth_pool = pool;
    } else {
        return TC_ERR;
    }

    pool = tc_create_pool(TC_PLUGIN_POOL_SIZE, TC_PLUGIN_POOL_SUB_SIZE, 0);
    if (pool) {
        ctx.sec_auth_pool = pool;
    } else {
        return TC_ERR;
    }

    pool = tc_create_pool(TC_PLUGIN_POOL_SIZE, TC_PLUGIN_POOL_SUB_SIZE, 0);
    if (pool) {
        ctx.ps_pool = pool;
    } else {
        return TC_ERR;
    }

    ctx.fir_auth_table = hash_create(ctx.fir_auth_pool, 65536);
    if (ctx.fir_auth_table == NULL) {
        return TC_ERR;
    }

    ctx.sec_auth_table = hash_create(ctx.sec_auth_pool, 65536);
    if (ctx.sec_auth_table == NULL) {
        return TC_ERR;
    }

    ctx.ps_table = hash_create(ctx.ps_pool, 65536);
    if (ctx.ps_table == NULL) {
        return TC_ERR;
    }

    return TC_OK;
}

static unsigned char *
copy_packet(tc_pool_t *pool, void *data)
{
    int            frame_len;
    uint16_t       tot_len;
    unsigned char *frame;
    
    tc_iph_t *ip = (tc_iph_t *) (((unsigned char *) data) + ETHERNET_HDR_LEN);
    tot_len   = ntohs(ip->tot_len);
    frame_len = ETHERNET_HDR_LEN + tot_len;

    frame = (unsigned char *) tc_palloc(pool, frame_len);

    if (frame != NULL) {    
        memcpy(frame + ETHERNET_HDR_LEN, ip, tot_len);
    }    

    return frame;
}


static int 
remove_or_refresh_fir_auth(uint64_t key, int is_refresh)
{
    void *value, *new_value;
    value = hash_find(ctx.fir_auth_table, key);

    if (value != NULL) {
        hash_del(ctx.fir_auth_table, ctx.fir_auth_pool, key);
        if (is_refresh) {
            new_value = copy_packet(ctx.fir_auth_pool, value);
            hash_add(ctx.fir_auth_table, ctx.fir_auth_pool, key, new_value);
#if (TC_DETECT_MEMORY)
            tc_log_info(LOG_INFO, 0, "refresh fir auth:%llu, new addr:%p",
                    key, new_value);
#endif
        }
#if (TC_DETECT_MEMORY)
        tc_log_info(LOG_INFO, 0, "free value:%p for key:%llu", value, key);
#endif
        tc_pfree(ctx.fir_auth_pool, value);
    }

    return TC_OK;
}


static int 
remove_or_refresh_sec_auth(uint64_t key, int is_refresh)
{
    void *value, *new_value;

    value = hash_find(ctx.sec_auth_table, key);
    if (value != NULL) {
        hash_del(ctx.sec_auth_table, ctx.sec_auth_pool, key);
        if (is_refresh) {
            new_value = copy_packet(ctx.sec_auth_pool, value);
            hash_add(ctx.sec_auth_table, ctx.sec_auth_pool, key, new_value);
#if (TC_DETECT_MEMORY)
            tc_log_info(LOG_INFO, 0, "refresh sec auth:%llu, new addr:%p", 
                    key, new_value);
#endif
        }
#if (TC_DETECT_MEMORY)
        tc_log_info(LOG_INFO, 0, "free value:%p for key:%llu", value, key);
#endif
        tc_pfree(ctx.sec_auth_pool, value);
    }

    return TC_OK;
}


static int 
remove_or_refresh_ps_stmt(uint64_t key, int is_refresh)
{
    void               *value, *pkt;
    link_list          *list;
    p_link_node         ln, tln, new_ln;
    mysql_table_item_t *item, *new_item;

    value = hash_find(ctx.ps_table, key);
    if (value != NULL) {

        item = value;

        if (is_refresh) {
            new_item = tc_pcalloc(ctx.ps_pool, sizeof(mysql_table_item_t));
            if (new_item != NULL) {
                new_item->list = link_list_create(ctx.ps_pool);
                if (new_item->list == NULL) {
                    tc_log_info(LOG_ERR, 0, "list create err");
                    is_refresh = 0;
                } else {
                    new_item->tot_cont_len = item->tot_cont_len;
                }
            } else {
                tc_log_info(LOG_ERR, 0, "mysql item create err");
                is_refresh = 0;
            }
        }

        list = item->list;
        ln   = link_list_first(list);
        while (ln) {
            tln = ln;
            ln = link_list_get_next(list, ln);
            link_list_remove(list, tln);
            if (is_refresh) {
                pkt = (unsigned char *) copy_packet(ctx.ps_pool, tln->data);
                new_ln  = link_node_malloc(ctx.ps_pool, pkt);
                new_ln->key = tln->key;
                link_list_append_by_order(new_item->list, new_ln);
#if (TC_DETECT_MEMORY)
                tc_log_info(LOG_INFO, 0, "refresh ps:%llu, new addr:%p, ln:%p",
                        key, pkt, new_ln);
#endif
            }
            tc_pfree(ctx.ps_pool, tln->data);
            tc_pfree(ctx.ps_pool, tln);
        }

        tc_pfree(ctx.ps_pool, value);
        tc_pfree(ctx.ps_pool, list);

        hash_del(ctx.ps_table, ctx.ps_pool, key);
                    
        if (is_refresh) {
            hash_add(ctx.ps_table, ctx.ps_pool, key, new_item);
#if (TC_DETECT_MEMORY)
            tc_log_info(LOG_INFO, 0, "refresh:%llu, new item:%p",
                    key, new_item);
#endif
        }
    }

    return TC_OK;
}


static int 
release_resources(uint64_t key)
{
    remove_or_refresh_fir_auth(key, 0);
    remove_or_refresh_sec_auth(key, 0);
    remove_or_refresh_ps_stmt(key, 0);

    return TC_OK;
}

static int 
refresh_resources(uint64_t key)
{
    remove_or_refresh_fir_auth(key, 1);
    remove_or_refresh_sec_auth(key, 1);
    remove_or_refresh_ps_stmt(key, 1);

    return TC_OK;
}

static void 
remove_table_obsolete_items(time_t thresh_access_tme) 
{
    uint32_t    i, cnt = 0;
    link_list  *l;
    hash_node  *hn;
    p_link_node ln, next_ln;

    if (ctx.fir_auth_table == NULL || ctx.fir_auth_table->total == 0) {
        return;
    }

    for (i = 0; i < ctx.fir_auth_table->size; i ++) {
        l  = get_link_list(ctx.fir_auth_table, i);
        if (l->size > 0) {
            ln = link_list_first(l);
            while (ln) {
                hn = (hash_node *) ln->data;
                next_ln = link_list_get_next(l, ln);
                if (hn->access_time < thresh_access_tme) {
                    tc_log_info(LOG_INFO, 0, 
                            "key:%llu, access time:%u, thresh_access_tme:%u",
                            hn->key, hn->access_time, thresh_access_tme);

                    release_resources(hn->key);
                }
                ln = next_ln;
            }

            cnt += l->size;

            if (ctx.fir_auth_table->total == cnt) {
                break;
            }
        }
    }
}


static void 
remove_obsolete_resources(int is_full) 
{
    time_t      thresh_access_tme;

    if (is_full) {
        thresh_access_tme = tc_time() + 1;
    } else {
        thresh_access_tme = tc_time() - MAX_IDLE_TIME;
    }

    remove_table_obsolete_items(thresh_access_tme);
}


static void 
exit_mysql_module() 
{
    tc_log_info(LOG_INFO, 0, "call exit_mysql_module");

    remove_obsolete_resources(1);

    if (ctx.fir_auth_pool != NULL) {
        tc_destroy_pool(ctx.fir_auth_pool);
        ctx.fir_auth_pool = NULL;
        ctx.fir_auth_table = NULL;
    }

    if (ctx.sec_auth_pool != NULL) {
        tc_destroy_pool(ctx.sec_auth_pool);
        ctx.sec_auth_pool = NULL;
        ctx.sec_auth_table = NULL;
    }

    if (ctx.ps_pool != NULL) {
        tc_destroy_pool(ctx.ps_pool);
        ctx.ps_pool = NULL;
        ctx.ps_table = NULL;
    }
}


static bool
check_renew_session(tc_iph_t *ip, tc_tcph_t *tcp)
{
    void           *value;
    uint16_t        size_ip, size_tcp, tot_len, cont_len;
    uint64_t        key;
    unsigned char  *payload, command, pack_number;

    if (ctx.fir_auth_table == NULL) {
        return false;
    }

    key   = get_key(ip->saddr, tcp->source);
    value = hash_find(ctx.fir_auth_table, key);
    if (value == NULL) {
        return false;
    }

    size_ip  = ip->ihl << 2;
    size_tcp = tcp->doff << 2;
    tot_len  = ntohs(ip->tot_len);
    cont_len = tot_len - size_tcp - size_ip;

    if (cont_len > 0) {
        payload = (unsigned char *) ((char *) tcp + size_tcp);
        /* skip packet length */
        payload = payload + 3;
        /* retrieve packet number */
        pack_number = payload[0];
        /* if it is the second authenticate_user, skip it */
        if (pack_number != 0) {
            return false;
        }
        /* skip packet number */
        payload = payload + 1;

        command = payload[0];
        tc_log_debug1(LOG_DEBUG, 0, "mysql command:%u", command);
        if (command == COM_QUERY || command == COM_STMT_EXECUTE) {
            return true;
        }
    }

    return false;
}
        

static bool 
check_pack_needed_for_recons(tc_sess_t *s, tc_iph_t *ip, tc_tcph_t *tcp)
{
    int                 diff;
    uint16_t            size_tcp;
    p_link_node         ln;
    unsigned char      *payload, command, *pkt;
    tc_mysql_session   *mysql_sess;
    mysql_table_item_t *item;

    mysql_sess = s->data;

    if (s->sm.fake_syn) {
        if (before(ntohl(tcp->seq), mysql_sess->seq_after_ps)) {
            return false;
        }
    }

    if (s->cur_pack.cont_len > 0) {

        size_tcp = tcp->doff << 2;
        payload = (unsigned char *) ((char *) tcp + size_tcp);
        /* skip packet length */
        payload  = payload + 3;
        /* skip packet number */
        payload  = payload + 1;
        command  = payload[0];

        if (command != COM_STMT_PREPARE) {
            
            diff = tc_time() - mysql_sess->last_refresh_time;

            if (diff >= MAX_RETHRESH_TIME) {
                refresh_resources(s->hash_key);
                mysql_sess->last_refresh_time = tc_time();
                mysql_sess->update_auth_table_item_switch = 0;
#if (TC_DETECT_MEMORY)
                tc_log_info(LOG_NOTICE, 0, "refresh time:%u for key:%llu", 
                        mysql_sess->last_refresh_time, s->hash_key);
#endif
            }

            mysql_sess->update_auth_table_item_switch++;
            if (mysql_sess->update_auth_table_item_switch == 0) {
                if (hash_find(ctx.fir_auth_table, s->hash_key) == NULL) {
                    tc_log_info(LOG_NOTICE, 0, "no fir auth for key:%llu", 
                            s->hash_key);
                }
            }

            return false;
        }

        item = hash_find(ctx.ps_table, s->hash_key);

        if (!item) {
            item = tc_pcalloc(ctx.ps_pool, sizeof(mysql_table_item_t));
            if (item != NULL) {
                item->list = link_list_create(ctx.ps_pool);
                if (item->list != NULL) {
                    hash_add(ctx.ps_table, ctx.ps_pool, s->hash_key, item);
                } else {
                    tc_log_info(LOG_ERR, 0, "list create err");
                    return false;
                }
            } else {
                tc_log_info(LOG_ERR, 0, "mysql item create err");
                return false;
            }
        }

        if (item->list->size > MAX_SP_SIZE) {
            tc_log_info(LOG_INFO, 0, "too many prepared stmts for a session");
            return false;
        }

        tc_log_debug1(LOG_INFO, 0, "push packet:%u", ntohs(s->src_port));

        pkt = (unsigned char *) cp_fr_ip_pack(ctx.ps_pool, ip);
        ln  = link_node_malloc(ctx.ps_pool, pkt);
        ln->key = ntohl(tcp->seq);
        link_list_append_by_order(item->list, ln);
        item->tot_cont_len += s->cur_pack.cont_len;

        return true;
    }

    return false;
}


static int
mysql_dispose_auth(tc_sess_t *s, tc_iph_t *ip, tc_tcph_t *tcp)
{
    int               auth_success;
    void             *value;
    char              encryption[ENCRYPT_LEN];
    uint16_t          size_tcp, cont_len;
    unsigned char    *payload;
    tc_mysql_session *mysql_sess;

    mysql_sess = s->data;

    size_tcp = tcp->doff << 2;
    cont_len = s->cur_pack.cont_len;

    if (!mysql_sess->first_auth_sent) {

        payload = (unsigned char *) ((char *) tcp + size_tcp);
        tc_log_debug1(LOG_INFO, 0, "change fir auth:%u", ntohs(s->src_port));
        auth_success = change_clt_auth_content(payload, (int) cont_len, 
               mysql_sess->map_user, mysql_sess->password, mysql_sess->scramble);

        if (!auth_success) {
            s->sm.sess_over  = 1; 
            tc_log_info(LOG_WARN, 0, "change fir auth unsuccessful");
            return TC_ERR;
        }

        mysql_sess->first_auth_sent = 1;

        if (!s->sm.fake_syn) {
            release_resources(s->hash_key);

            value = (void *) cp_fr_ip_pack(ctx.fir_auth_pool, ip);
            hash_add(ctx.fir_auth_table, ctx.fir_auth_pool, s->hash_key, value);
            mysql_sess->last_refresh_time = tc_time();

#if (TC_DETECT_MEMORY)
            tc_log_info(LOG_INFO, 0, "s:%p,hash add fir auth:%llu,value:%p, p:%u",
                    s, s->hash_key, value, ntohs(s->src_port));
#endif
        } else {
            hash_node *hn = hash_find_node(ctx.fir_auth_table, s->hash_key);
            if (hn != NULL) {
                mysql_sess->last_refresh_time = hn->create_time;
            } else {
                tc_log_info(LOG_WARN, 0, "hash node for key:%llu is nil", 
                        s->hash_key);
            }
        }

    } else if (mysql_sess->first_auth_sent && mysql_sess->sec_auth_not_yet_done)
    {
        payload = (unsigned char *) ((char *) tcp + size_tcp);

        tc_memzero(encryption, ENCRYPT_LEN);
        tc_memzero(mysql_sess->seed323, SEED_323_LENGTH + 1);
        memcpy(mysql_sess->seed323, mysql_sess->scramble, SEED_323_LENGTH);
        new_crypt(encryption, mysql_sess->password, mysql_sess->seed323);

        tc_log_debug1(LOG_INFO, 0, "change sec auth:%u", ntohs(s->src_port));
        change_clt_second_auth_content(payload, cont_len, encryption);
        mysql_sess->sec_auth_not_yet_done = 0;

        if (!s->sm.fake_syn) {
            value = (void *) cp_fr_ip_pack(ctx.sec_auth_pool, ip);
            hash_add(ctx.sec_auth_table, ctx.sec_auth_pool, s->hash_key, value);
        }
    }

    return TC_OK;
}


static int 
prepare_for_renew_session(tc_sess_t *s, tc_iph_t *ip, tc_tcph_t *tcp)
{
    uint16_t            size_ip, fir_clen, sec_clen;
    uint32_t            tot_clen, base_seq;
    uint64_t            key;
    tc_iph_t           *fir_ip, *t_ip, *sec_ip;
    tc_tcph_t          *fir_tcp, *t_tcp, *sec_tcp;
    p_link_node         ln;
    unsigned char      *p;
    mysql_table_item_t *item;
    tc_mysql_session   *mysql_sess;

    mysql_sess = s->data;
    if (mysql_sess == NULL) {
        tc_log_info(LOG_WARN, 0, "mysql session structure is not allocated");
        return TC_ERR;
    } else if (mysql_sess->auth_packet_already_added) {
        tc_log_info(LOG_NOTICE, 0, "dup visit prepare_for_renew_session");
        return TC_OK;
    }

    sec_ip = NULL;
    sec_tcp = NULL;
    s->sm.need_rep_greet = 1;

    key = s->hash_key;

    p = (unsigned char *) hash_find(ctx.fir_auth_table, key);

    if (p != NULL) {
        fir_ip   = (tc_iph_t *) (p + ETHERNET_HDR_LEN);
        size_ip  = fir_ip->ihl << 2;
        fir_tcp  = (tc_tcph_t *) ((char *) fir_ip + size_ip);
        fir_clen = TCP_PAYLOAD_LENGTH(fir_ip, fir_tcp);
        tot_clen = fir_clen;
    } else {
        tc_log_info(LOG_WARN, 0, "no first auth:%u", ntohs(s->src_port));
        return TC_ERR;
    }

    p = (unsigned char *) hash_find(ctx.sec_auth_table, key);
    if (p != NULL) {
        sec_ip    = (tc_iph_t *) (p + ETHERNET_HDR_LEN);
        size_ip   = sec_ip->ihl << 2;
        sec_tcp   = (tc_tcph_t *) ((char *) sec_ip + size_ip);
        sec_clen  = TCP_PAYLOAD_LENGTH(sec_ip, sec_tcp);
        tot_clen += sec_clen;
    } else {
        sec_clen  = 0;
        tc_log_debug1(LOG_INFO, 0, "no sec auth:%u", ntohs(s->src_port));
    }

    item = hash_find(ctx.ps_table, s->hash_key);
    if (item) {
        tot_clen += item->tot_cont_len;
    }

    tc_log_debug2(LOG_INFO, 0, "total len subtracted:%u,p:%u", tot_clen,
            ntohs(s->src_port));

    mysql_sess->seq_after_ps = ntohl(tcp->seq);

    tcp->seq     = htonl(ntohl(tcp->seq) - tot_clen);
    fir_tcp->seq = htonl(ntohl(tcp->seq) + 1);
    tc_save_pack(s, s->slide_win_packs, fir_ip, fir_tcp);  
    mysql_sess->auth_packet_already_added = 1;

    if (sec_tcp != NULL) {
        sec_tcp->seq = htonl(ntohl(fir_tcp->seq) + fir_clen);
        tc_save_pack(s, s->slide_win_packs, sec_ip, sec_tcp);
        tc_log_debug1(LOG_INFO, 0, "add sec auth:%u", ntohs(s->src_port));
    }

    base_seq = ntohl(fir_tcp->seq) + fir_clen + sec_clen;

    if (item) {
        ln = link_list_first(item->list); 
        while (ln) {
            p = (unsigned char *) ln->data;
            t_ip  = (tc_iph_t *) (p + ETHERNET_HDR_LEN);
            t_tcp = (tc_tcph_t *) ((char *) t_ip + size_ip);
            t_tcp->seq = htonl(base_seq);
            tc_save_pack(s, s->slide_win_packs, t_ip, t_tcp);  
            base_seq += TCP_PAYLOAD_LENGTH(t_ip, t_tcp);
            ln = link_list_get_next(item->list, ln);
        }
    }

    return TC_OK;
}


static int 
proc_when_sess_created(tc_sess_t *s)
{
    tc_mysql_session *data = s->data;

    if (data == NULL) {
        data = (tc_mysql_session *) tc_pcalloc(s->pool, sizeof(tc_mysql_session));

        if (data) {
            s->data = data;
        }
    } else {
        tc_memzero(data, sizeof(tc_mysql_session)); 
    }

    return TC_OK;
}


static int 
proc_when_sess_destroyed(tc_sess_t *s)
{
    release_resources(s->hash_key);
    return TC_OK;
}


static int
proc_greet(tc_sess_t *s, tc_iph_t *ip, tc_tcph_t *tcp)
{
    int               ret; 
    uint16_t          size_tcp, cont_len; 
    unsigned char    *payload;
    tc_mysql_session *mysql_sess;

    mysql_sess = s->data;
    tc_log_debug1(LOG_INFO, 0, "recv greet from back:%u", ntohs(s->src_port));
    size_tcp = tcp->doff << 2;
    mysql_sess->sec_auth_checked  = 0;
    payload = (unsigned char *) ((char *) tcp + size_tcp);
    tc_memzero(mysql_sess->scramble, SCRAMBLE_LENGTH + 1);

    cont_len = s->cur_pack.cont_len;

    ret = parse_handshake_init_cont(payload, cont_len, mysql_sess->scramble);
    if (!ret) {
        if (cont_len > 11) {
            tc_log_info(LOG_WARN, 0, "port:%u,payload:%s",
                    ntohs(s->src_port), (char *) (payload + 11));
        }
        s->sm.sess_over = 1;
        return PACK_STOP;
    }

    return PACK_CONTINUE;
}


static int 
check_needed_for_sec_auth(tc_sess_t *s, tc_iph_t *ip, tc_tcph_t *tcp)
{
    uint16_t          size_tcp;
    unsigned char    *payload;
    tc_mysql_session *mysql_sess;

    mysql_sess = s->data;
    if (mysql_sess->sec_auth_checked == 0) {
        size_tcp = tcp->doff << 2;
        payload = (unsigned char *) ((char *) tcp + size_tcp);
        if (is_last_data_packet(payload)) {
            tc_log_debug1(LOG_INFO, 0, "needs sec auth:%u", ntohs(s->src_port));
            mysql_sess->sec_auth_not_yet_done = 1;
        }
        mysql_sess->sec_auth_checked = 1;
    }

    return TC_OK;
}


static int 
proc_auth(tc_sess_t *s, tc_iph_t *ip, tc_tcph_t *tcp)
{
    if (!s->sm.rcv_rep_greet) {
        return PACK_STOP;
    }

    if (mysql_dispose_auth(s, ip, tcp) == TC_ERR) {
        return PACK_STOP;
    }

    return PACK_CONTINUE;
}


static int
mysql_parse_user_info(tc_conf_t *cf, tc_cmd_t *cmd)
{
    char       pass[MAX_USER_INFO];
    tc_str_t  *user_password;

    user_password = cf->args->elts;

    tc_memzero(pass, MAX_USER_INFO);

    if (user_password[1].len >= MAX_USER_INFO) {
        tc_log_info(LOG_ERR, 0, "user password pair too long");
        return TC_ERR;
    }

    memcpy(pass, user_password[1].data, user_password[1].len);

    if (retrieve_mysql_user_pwd_info(cf->pool, pass) == -1) {
        tc_log_info(LOG_ERR, 0, "parse user password error");
        return TC_ERR;
    }

    return TC_OK;
}


static tc_cmd_t  mysql_commands[] = {
    { tc_string("user"),
        0,
        0,
        TC_CONF_TAKE1,
        mysql_parse_user_info,
        NULL
    }
};


tc_module_t tc_mysql_module = {
    &ctx,
    mysql_commands,
    init_mysql_module,
    exit_mysql_module,
    remove_obsolete_resources,
    check_renew_session,
    prepare_for_renew_session,
    check_pack_needed_for_recons,
    proc_when_sess_created,
    proc_when_sess_destroyed,
    release_resources,
    proc_greet,
    proc_auth,
    check_needed_for_sec_auth,
    NULL
};

