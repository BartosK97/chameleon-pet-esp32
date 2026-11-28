// HTTPS calls to the Claude Messages API and ntfy.sh. TLS needs ~45 KB of internal RAM, so callers switch BLE off first.
#pragma once
#include <Arduino.h>

bool claude_ask(const char *key, const char *system, const String &user, int max_tokens, String &reply);
extern uint32_t ai_calls, ai_in, ai_out;  // since boot, from the API's usage; Haiku 5.5 bills $0.10 / $0.50 per million
bool ntfy_push(const char *topic, const char *title, const char *msg);
