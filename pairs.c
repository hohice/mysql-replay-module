
#include <xcopy.h>
#include "pairs.h"

static hash_table *user_pwd_table = NULL;
static hash_table *user_map_table = NULL;

static uint64_t
get_key_from_user(char *user)
{
    size_t   len, i;
    uint64_t key = 0;

    if (user == NULL) {
        return key;
    }

    len = strlen(user);
    for (i = 0; i < len; i++ ) {   
        key = 31*key + user[i];
    }   

    return key;
}

char* 
retrieve_user_map(char *user)
{
    uint64_t    key;
    mysql_user *p_user_info;

    if (user_pwd_table == NULL) {
        tc_log_info(LOG_ERR, 0, "empty user info in conf/plugin.conf");
        fprintf(stderr, "empty user info in conf/plugin.conf\n");
        return NULL;
    }

    key         = get_key_from_user(user);
    p_user_info = hash_find(user_pwd_table, key);

    while (p_user_info) {
        if (strcmp(p_user_info->user, user) == 0) {
            return p_user_info->map_user;
        }
        p_user_info = p_user_info->next;
    }

    return NULL;
}
char *
retrieve_user_pwd(char *user)
{
    uint64_t    key;
    mysql_user *p_user_info;

    if (user_pwd_table == NULL) {
        tc_log_info(LOG_ERR, 0, "empty user info in conf/plugin.conf");
        fprintf(stderr, "empty user info in conf/plugin.conf\n");
        return NULL;
    }

    key         = get_key_from_user(user);
    p_user_info = hash_find(user_pwd_table, key);

    while (p_user_info) {
        if (strcmp(p_user_info->user, user) == 0) {
            return p_user_info->password;
        }
        p_user_info = p_user_info->next;
    }

    return NULL;
}

int 
retrieve_mysql_user_pwd_info(tc_pool_t *pool, char *pairs)
{
    char       *p, *end, *q, *next, *pair_end,user_temp[256+256],*to_user,map_user[256];
    size_t      len;  
    uint64_t    key;
    mysql_user *p_user_info, *p_tmp_user_info;
    
    user_pwd_table = hash_create(pool, 256);
    user_pwd_table->pool = pool;

    p   = pairs;
    len = strlen(p);
    end = p + len;

    if (len <= 1) {
        tc_log_info(LOG_WARN, 0, "use password error:%s:", pairs);
        return -1;
    }

    do {
        next = strchr(p, ',');
        q = strchr(p, ':');

        if (next != NULL) {
            if (next != p) {
                pair_end = next - 1;
            } else {
                tc_log_info(LOG_WARN, 0, "use password error:%s:", pairs);
                return -1;
            }
        } else {
            pair_end = p + strlen(p) - 1;
        }

        if (q == NULL || pair_end <= q) {
            tc_log_info(LOG_WARN, 0, "user without password is found");
            return -1;
        }

        if ((q - p) >= 256 || (pair_end - q) >= 256) {
            tc_log_info(LOG_WARN, 0, "too long for user or password");
            return -1;
        }

        p_user_info = (mysql_user *) tc_pcalloc(pool, sizeof(mysql_user));
        if (p_user_info == NULL) {
            return -1;
        }

        
//user_temp
        strncpy(user_temp, p, q - p);
        to_user = strchr(p, '#');
        if (next != NULL) {
            strncpy(p_user_info->map_user, to_user+1, q - to_user);
            strncpy(p_user_info->user, p, to_user-p);
        }else{
            strncpy(p_user_info->user, p, q - p);
        }

        


        strncpy(p_user_info->password, q + 1, pair_end - q);
        key = get_key_from_user(p_user_info->user);
        p_tmp_user_info = hash_find(user_pwd_table, key);

        if (p_tmp_user_info == NULL) {
            hash_add(user_pwd_table, pool, key, (void *) p_user_info);
        } else {
            p_tmp_user_info->next = p_user_info;
        }

        if (next != NULL) {
            p = next + 1;
        } else {
            break;
        }
    } while (p < end);

    return 0;
}

