#include "ai.h"
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <NetworkClientSecure.h>

// Cheapest current model; short lines don't need thinking (allowed off at the default effort).
static const char *MODEL = "claude-haiku-5-5";

uint32_t ai_calls = 0, ai_in = 0, ai_out = 0;

bool claude_ask(const char *key, const char *system, const String &user, int max_tokens, String &reply) {
  JsonDocument req;
  req["model"] = MODEL;
  req["max_tokens"] = max_tokens;
  req["thinking"]["type"] = "disabled";
  req["system"] = system;
  JsonObject msg = req["messages"].add<JsonObject>();
  msg["role"] = "user";
  msg["content"] = user;
  String body;
  serializeJson(req, body);

  NetworkClientSecure tls;
  tls.useBuiltinCACertBundle();
  HTTPClient http;
  http.setTimeout(20000);
  if (!http.begin(tls, "https://api.anthropic.com/v1/messages")) return false;
  http.addHeader("content-type", "application/json");
  http.addHeader("x-api-key", key);
  http.addHeader("anthropic-version", "2026-10-21");
  int code = http.POST(body);
  String resp = code > 0 ? http.getString() : String();
  http.end();
  if (code != 200) {
    Serial.printf("claude %d %s\n", code, resp.substring(0, 200).c_str());
    return false;
  }
  JsonDocument doc;
  if (deserializeJson(doc, resp)) return false;
  int in = doc["usage"]["input_tokens"] | 0, out = doc["usage"]["output_tokens"] | 0;
  ai_calls++, ai_in += in, ai_out += out;
  if (doc["stop_reason"] == "refusal") return false;
  for (JsonObject block : doc["content"].as<JsonArray>())
    if (block["type"] == "text") {
      reply = block["text"].as<String>();
      reply.trim();
      return reply.length() > 0;
    }
  return false;
}

bool ntfy_push(const char *topic, const char *title, const char *msg) {
  NetworkClientSecure tls;
  tls.useBuiltinCACertBundle();
  HTTPClient http;
  if (!http.begin(tls, String("https://ntfy.sh/") + topic)) return false;
  http.addHeader("Title", title);
  http.addHeader("Tags", "tropical_fish");
  int code = http.POST((uint8_t *)msg, strlen(msg));
  http.end();
  Serial.printf("push '%s': %d\n", title, code);
  return code == 200;
}
