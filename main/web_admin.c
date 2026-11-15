#include "sdkconfig.h"
#if CONFIG_PSP_HTTP_PROXY
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "mbedtls/constant_time.h"
#include "mbedtls/platform_util.h"
#include "nvs.h"
#include "psa/crypto.h"
#include "bridge_policy.h"
#include "router_settings.h"
#include "usb_net.h"
#include "web_admin.h"

#define PASSWORD_COST 40000
#define TOKEN_LENGTH PROXY_ADMIN_TOKEN_LENGTH
#define SESSION_COUNT 4
#define SESSION_LIFETIME_US (60LL * 60 * 1000000)
typedef struct {
    uint8_t salt[16];
    uint8_t digest[32];
} admin_password_t;

typedef struct {
    char id[TOKEN_LENGTH + 1];
    char csrf[TOKEN_LENGTH + 1];
    int64_t expires_at;
} admin_session_t;

static SemaphoreHandle_t s_lock;
static bool s_ready, s_default_password, s_restart_pending;
static bool s_wifi_pending, s_wifi_apply_failed;
static admin_password_t s_password;
static admin_session_t s_sessions[SESSION_COUNT];
static router_wifi_settings_t s_pending_wifi;

static void random_token(char token[TOKEN_LENGTH + 1])
{
    static const char hex[] = "0123456789abcdef";
    uint8_t random[TOKEN_LENGTH / 2];
    esp_fill_random(random, sizeof(random));
    for (size_t i = 0; i < sizeof(random); ++i) {
        token[2 * i] = hex[random[i] >> 4];
        token[2 * i + 1] = hex[random[i] & 15];
    }
    token[TOKEN_LENGTH] = '\0';
}

void web_admin_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock || psa_crypto_init() != PSA_SUCCESS) return;
    nvs_handle_t nvs;
    esp_err_t result = nvs_open("psp_config", NVS_READONLY, &nvs);
    if (result == ESP_OK) {
        size_t length = sizeof(s_password);
        result = nvs_get_blob(nvs, "admin_v1", &s_password, &length);
        nvs_close(nvs);
        if (result == ESP_OK && length != sizeof(s_password)) return;
    }
    if (result != ESP_OK && result != ESP_ERR_NVS_NOT_FOUND) return;
    s_default_password = result == ESP_ERR_NVS_NOT_FOUND;
    s_ready = true;
}

static bool derive(const uint8_t *password, size_t length, const uint8_t salt[16], uint8_t digest[32])
{
    psa_key_derivation_operation_t operation = PSA_KEY_DERIVATION_OPERATION_INIT;
    psa_status_t status = psa_key_derivation_setup(&operation, PSA_ALG_PBKDF2_HMAC(PSA_ALG_SHA_256));
    if (status == PSA_SUCCESS) status = psa_key_derivation_input_integer(&operation, PSA_KEY_DERIVATION_INPUT_COST, PASSWORD_COST);
    if (status == PSA_SUCCESS) status = psa_key_derivation_input_bytes(&operation, PSA_KEY_DERIVATION_INPUT_SALT, salt, 16);
    if (status == PSA_SUCCESS) status = psa_key_derivation_input_bytes(&operation, PSA_KEY_DERIVATION_INPUT_PASSWORD, password, length);
    if (status == PSA_SUCCESS) status = psa_key_derivation_output_bytes(&operation, digest, 32);
    psa_key_derivation_abort(&operation);
    return status == PSA_SUCCESS;
}

static bool password_matches(const char *password)
{
    size_t length = strlen(password);
    if (s_default_password) {
        return length == 5 && mbedtls_ct_memcmp(password, "admin", 5) == 0;
    }
    uint8_t digest[32];
    bool valid = derive((const uint8_t *)password, length, s_password.salt, digest) &&
                 mbedtls_ct_memcmp(digest, s_password.digest, sizeof(digest)) == 0;
    mbedtls_platform_zeroize(digest, sizeof(digest));
    return valid;
}

static bool save_password(const char *password)
{
    admin_password_t next;
    esp_fill_random(next.salt, sizeof(next.salt));
    if (!derive((const uint8_t *)password, strlen(password), next.salt, next.digest)) return false;
    nvs_handle_t nvs;
    if (nvs_open("psp_config", NVS_READWRITE, &nvs) != ESP_OK) return false;
    esp_err_t result = nvs_set_blob(nvs, "admin_v1", &next, sizeof(next));
    if (result == ESP_OK) result = nvs_commit(nvs);
    nvs_close(nvs);
    if (result != ESP_OK) return false;
    s_password = next;
    s_default_password = false;
    mbedtls_platform_zeroize(s_sessions, sizeof(s_sessions));
    return true;
}

static const char page_start[] =
    "<!doctype html><html lang='en'><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>PSP Router Admin</title><style>"
    "body{font:1rem system-ui,sans-serif;max-width:36rem;margin:2rem auto;padding:0 1rem}"
    "label{display:block;margin:.75rem 0}input{display:block;max-width:100%;font:inherit}"
    "button{font:inherit;padding:.35rem .65rem}fieldset{margin:1rem 0;padding:1rem}"
    "</style></head><body><h1>PSP Router Admin</h1>";

static void feedback(web_admin_reply_t *reply, int status, const char *message)
{
    reply->status = status;
    bool show_home = reply->action != WEB_ADMIN_ACTION_NONE || status == 401;
    snprintf(reply->html, sizeof(reply->html), "%s<p>%s</p><p><a href='%s'>%s</a></p></body></html>",
             page_start, message, show_home ? "/" : "/admin/", show_home ? "Proxy home" : "Back to settings");
}

static void login_page(web_admin_reply_t *reply, int status, const char *message)
{
    random_token(reply->cookie);
    reply->status = status;
    snprintf(reply->html, sizeof(reply->html),
        "%s<p>%s</p><form method='post' action='/admin/login'>"
        "<input type='hidden' name='token' value='%s'>"
        "<label>Username<input name='username' maxlength='32' autocomplete='username' required></label>"
        "<label>Password<input type='password' name='password' maxlength='63' autocomplete='current-password' required></label>"
        "<input type='submit' value='Log in'></form><p><a href='/'>Proxy home</a></p></body></html>",
        page_start, message, reply->cookie);
}

static bool token_matches(const char *token, const char *expected)
{
    return strlen(token) == TOKEN_LENGTH && mbedtls_ct_memcmp(token, expected, TOKEN_LENGTH) == 0;
}

static admin_session_t *find_session(const char *id)
{
    int64_t now = esp_timer_get_time();
    for (size_t i = 0; i < SESSION_COUNT; ++i) {
        if (s_sessions[i].expires_at > now && token_matches(id, s_sessions[i].id)) return &s_sessions[i];
    }
    return NULL;
}

static void login(char *body, const char *cookie, admin_session_t *session, web_admin_reply_t *reply)
{
    char token[TOKEN_LENGTH + 1], user[33], password[64];
    const proxy_form_field_t fields[] = {
        {"token", token, sizeof(token)}, {"username", user, sizeof(user)}, {"password", password, sizeof(password)},
    };
    if (session) {
        reply->status = 302;
        reply->location = "/admin/";
    } else if (!proxy_parse_form(body, fields, sizeof(fields) / sizeof(fields[0])) ||
               !cookie[0] || !token_matches(token, cookie)) {
        login_page(reply, 403, "Reload the page and allow cookies before signing in.");
    } else if (strcmp(user, "admin") || !password_matches(password)) {
        login_page(reply, 403, "Invalid username or password.");
    } else {
        admin_session_t *slot = &s_sessions[0];
        for (size_t i = 1; i < SESSION_COUNT; ++i) {
            if (s_sessions[i].expires_at < slot->expires_at) slot = &s_sessions[i];
        }
        random_token(slot->id);
        random_token(slot->csrf);
        slot->expires_at = esp_timer_get_time() + SESSION_LIFETIME_US;
        memcpy(reply->cookie, slot->id, sizeof(reply->cookie));
        reply->status = 302;
        reply->location = "/admin/";
    }
    mbedtls_platform_zeroize(password, sizeof(password));
}

static void settings_page(web_admin_reply_t *reply, const char *csrf)
{
    router_wifi_settings_t wifi;
    router_wifi_settings_load(&wifi);
    router_wifi_settings_get_active(&wifi);
    char ssid[32 * 6 + 1];
    char *output = ssid;
    for (const char *p = wifi.ssid; *p; ++p) {
        const char *entity = NULL;
        switch (*p) {
        case '&': entity = "&amp;"; break;
        case '<': entity = "&lt;"; break;
        case '>': entity = "&gt;"; break;
        case '\'': entity = "&#39;"; break;
        case '"': entity = "&quot;"; break;
        default: *output++ = *p; break;
        }
        if (entity) {
            size_t length = strlen(entity);
            memcpy(output, entity, length);
            output += length;
        }
    }
    *output = '\0';
    mbedtls_platform_zeroize(&wifi, sizeof(wifi));
    int length = snprintf(reply->html, sizeof(reply->html),
        "%s<form method='POST' action='/admin/logout'>"
        "<input type='hidden' name='token' value='%s'><input type='submit' value='Logout'></form>"
        "<p><a href='/'>Proxy home</a></p>%s"
        "<form method='POST' action='/admin/wifi'><fieldset><legend>Wi-Fi</legend>"
        "<input type='hidden' name='token' value='%s'>"
        "<label>Network name<input name='ssid' value='%s' required maxlength='32'></label>"
#if !CONFIG_PSP_AP_OPEN
        "<label>New Wi-Fi password<input type='password' name='wifi_password' maxlength='63' autocomplete='new-password'></label>"
        "<p>Use 8 to 63 printable ASCII characters or leave blank to keep the current password.</p>"
#endif
        "<button type='submit'>Save Wi-Fi</button></fieldset></form>"
        "<form method='POST' action='/admin/password'><fieldset><legend>Administrator</legend>"
        "<input type='hidden' name='token' value='%s'>"
        "<label>New admin password<input type='password' name='password' required minlength='8' maxlength='63' autocomplete='new-password'></label>"
        "<label>Confirm password<input type='password' name='confirm' required minlength='8' maxlength='63' autocomplete='new-password'></label>"
        "<button type='submit'>Change admin password</button></fieldset></form>"
        "<form method='POST' action='/admin/bootloader'><fieldset><legend>Firmware</legend>"
        "<input type='hidden' name='token' value='%s'>"
        "<p>USB networking and Wi-Fi stop until you flash the firmware or press RESET.</p>"
        "<button type='submit'>Reboot to bootloader</button></fieldset></form></body></html>",
        page_start, csrf, s_wifi_apply_failed ? "<p>The last Wi-Fi update failed. Check the active settings below.</p>" : "",
        csrf, ssid, csrf, csrf);
    if (length < 0 || (size_t)length >= sizeof(reply->html)) {
        feedback(reply, 500, "Settings page unavailable.");
        return;
    }
    reply->status = 200;
}

static void change_wifi(char *body, const char *csrf, web_admin_reply_t *reply)
{
    router_wifi_settings_t wifi = {0};
    char token[TOKEN_LENGTH + 1];
    const proxy_form_field_t fields[] = {
        {"token", token, sizeof(token)},
        {"ssid", wifi.ssid, sizeof(wifi.ssid)},
#if !CONFIG_PSP_AP_OPEN
        {"wifi_password", wifi.password, sizeof(wifi.password)},
#endif
    };
    if (!proxy_parse_form(body, fields, sizeof(fields) / sizeof(fields[0]))) {
        feedback(reply, 400, "Invalid Wi-Fi form.");
    } else if (!token_matches(token, csrf)) {
        feedback(reply, 403, "Reload the settings page and try again.");
    } else if (!bridge_ssid_valid(wifi.ssid) || (wifi.password[0] && !bridge_password_valid(wifi.password))) {
        feedback(reply, 400, "Use 1 to 32 printable ASCII characters for the network name and 8 to 63 for the password.");
    } else {
        router_wifi_settings_t previous;
        router_wifi_settings_load(&previous);
        router_wifi_settings_get_active(&previous);
        if (!wifi.password[0]) memcpy(wifi.password, previous.password, sizeof(wifi.password));
        if (!strcmp(wifi.ssid, previous.ssid) && !strcmp(wifi.password, previous.password)) {
            feedback(reply, 200, "Wi-Fi settings are already up to date.");
        } else {
            s_pending_wifi = wifi;
            s_wifi_pending = true;
            reply->action = WEB_ADMIN_APPLY_WIFI;
            feedback(reply, 202, "Applying Wi-Fi settings. Reconnect using the new settings.");
        }
        mbedtls_platform_zeroize(&previous, sizeof(previous));
    }
    mbedtls_platform_zeroize(&wifi, sizeof(wifi));
}

void web_admin_apply_pending_wifi(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_wifi_apply_failed = router_wifi_settings_apply(&s_pending_wifi) != ESP_OK;
    mbedtls_platform_zeroize(&s_pending_wifi, sizeof(s_pending_wifi));
    s_wifi_pending = false;
    xSemaphoreGive(s_lock);
}

static void change_password(char *body, const char *csrf, web_admin_reply_t *reply)
{
    char token[TOKEN_LENGTH + 1], password[64], confirm[64];
    const proxy_form_field_t fields[] = {
        {"token", token, sizeof(token)},
        {"password", password, sizeof(password)},
        {"confirm", confirm, sizeof(confirm)},
    };
    if (!proxy_parse_form(body, fields, sizeof(fields) / sizeof(fields[0]))) {
        feedback(reply, 400, "Invalid password form.");
    } else if (!token_matches(token, csrf)) {
        feedback(reply, 403, "Reload the settings page and try again.");
    } else if (!bridge_password_valid(password) || strcmp(password, confirm)) {
        feedback(reply, 400, "Passwords must match and contain 8 to 63 printable ASCII characters.");
    } else if (!save_password(password)) {
        feedback(reply, 500, "Could not save the admin password.");
    } else {
        reply->clear_cookie = true;
        feedback(reply, 200, "Admin password changed. Sign in again using the new password.");
    }
    mbedtls_platform_zeroize(password, sizeof(password));
    mbedtls_platform_zeroize(confirm, sizeof(confirm));
}

static bool check_action_token(char *body, const char *csrf, web_admin_reply_t *reply)
{
    char token[TOKEN_LENGTH + 1];
    const proxy_form_field_t field = {"token", token, sizeof(token)};
    if (!proxy_parse_form(body, &field, 1)) {
        feedback(reply, 400, "Invalid settings request.");
    } else if (!token_matches(token, csrf)) {
        feedback(reply, 403, "Reload the settings page and try again.");
    } else {
        return true;
    }
    return false;
}

static void reboot_to_bootloader(char *body, const char *csrf, web_admin_reply_t *reply)
{
    if (!check_action_token(body, csrf, reply)) return;
    if (!usb_net_rom_available()) {
        feedback(reply, 409, "This chip does not allow a software reboot to USB serial download mode.");
    } else {
        s_restart_pending = true;
        reply->action = WEB_ADMIN_RESTART_ROM;
        feedback(reply, 200, "Rebooting to the ROM bootloader. Select the USB serial port in your flasher.");
    }
}

static void logout(char *body, admin_session_t *session, web_admin_reply_t *reply)
{
    if (!check_action_token(body, session->csrf, reply)) return;
    mbedtls_platform_zeroize(session, sizeof(*session));
    reply->clear_cookie = true;
    reply->status = 302;
    reply->location = "/admin/";
}

void web_admin_handle(const proxy_request_t *request, proxy_admin_route_t route,
                      char *body, web_admin_reply_t *reply)
{
    if (!s_ready || xSemaphoreTake(s_lock, pdMS_TO_TICKS(2000)) != pdTRUE) {
        feedback(reply, 503, "Admin settings are temporarily unavailable.");
        return;
    }
    char cookie[TOKEN_LENGTH + 1] = {0};
    if (!proxy_admin_cookie(request->cookie, cookie)) cookie[0] = '\0';
    admin_session_t *session = cookie[0] ? find_session(cookie) : NULL;
    if (s_restart_pending) {
        feedback(reply, 503, "The router is restarting.");
    } else if (s_wifi_pending) {
        feedback(reply, 503, "Wi-Fi settings are being applied. Try again in a moment.");
    } else if (route == PROXY_ADMIN_PAGE) {
        if (session) settings_page(reply, session->csrf);
        else login_page(reply, 200, "Sign in to change router settings.");
    } else if (!request->post) {
        feedback(reply, 405, "Use the form on the settings page.");
    } else if (route == PROXY_ADMIN_LOGIN) {
        login(body, cookie, session, reply);
    } else if (!session) {
        feedback(reply, 401, "Administrator sign-in required.");
    } else if (route == PROXY_ADMIN_LOGOUT) {
        logout(body, session, reply);
    } else if (route == PROXY_ADMIN_WIFI) {
        change_wifi(body, session->csrf, reply);
    } else if (route == PROXY_ADMIN_PASSWORD) {
        change_password(body, session->csrf, reply);
    } else if (route == PROXY_ADMIN_BOOTLOADER) {
        reboot_to_bootloader(body, session->csrf, reply);
    }
    xSemaphoreGive(s_lock);
}
#endif
