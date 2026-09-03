/**
 * @file hxd019_session.c
 * @brief HXD019EU 会话状态机实现（core 层）
 */
#include <string.h>
#include "hxd019_session.h"
#include "hxd019_frame.h"

void hxd019_session_init(hxd019_session_t *s, hxd019_fcode_hook_t hook)
{
    if (s == NULL) {
        return;
    }
    memset(s, 0, sizeof(*s));
    s->fcode_hook = hook;  /* NULL = 默认桩行为（普通 F_code 直发） */
}

void hxd019_session_bind(hxd019_session_t *s, uint16_t code_group)
{
    if (s == NULL) {
        return;
    }
    s->code_group = code_group;
    s->has_state = false;   /* 换组后旧状态无意义 */
    memset(&s->last_state, 0, sizeof(s->last_state));
}

uint16_t hxd019_session_group(const hxd019_session_t *s)
{
    return (s == NULL) ? 0 : s->code_group;
}

void hxd019_session_base_state(hxd019_session_t *s, hxd019_ac_state_t *out, bool *is_default)
{
    if (out == NULL) {
        return;
    }
    if (s != NULL && s->has_state) {
        *out = s->last_state;
        if (is_default != NULL) {
            *is_default = false;
        }
    } else {
        hxd019_ac_state_default(out);
        if (is_default != NULL) {
            *is_default = true;
        }
    }
}

void hxd019_session_commit(hxd019_session_t *s, const hxd019_ac_state_t *st)
{
    if (s == NULL || st == NULL) {
        return;
    }
    s->last_state = *st;
    s->has_state = true;
}

void hxd019_session_unbind(hxd019_session_t *s)
{
    hxd019_session_bind(s, 0);
}
