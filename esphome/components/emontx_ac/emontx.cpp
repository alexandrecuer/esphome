#include <string>
#include <map>
#include <cctype>

#include "emontx.h"
#include "esphome/core/log.h"
#include "esphome/core/application.h"
#include "esphome/core/helpers.h"
#include "esphome/components/json/json_util.h"

namespace esphome::emontx {

static const char *const TAG = "emontx";

std::string guess_device_class(const std::string &tag) {
  if (tag[0] == 'P')
    return "power";
  if (tag[0] == 'E')
    return "energy";
  if (tag[0] == 'V')
    return "voltage";
  if (tag == "F")
    return "frequency";
  return "";
}

std::string guess_unit(const std::string &tag) {
  if (tag[0] == 'P')
    return "W";
  if (tag[0] == 'V')
    return "V";
  if (tag == "F")
    return "Hz";
  return "";
}

std::string guess_state_class(const std::string &tag) {
  if (tag[0] == 'E')
    return "total_increasing";
  return "measurement";
}

ParsedData parseTrame(const std::string &trame) {
  ParsedData result;

  size_t pos = 0;

  while (pos < trame.size()) {
    size_t comma = trame.find(',', pos);
    if (comma == std::string::npos) {
      comma = trame.size();
    }

    size_t colon = trame.find(':', pos);

    if (colon != std::string::npos && colon < comma) {
      std::string key = trame.substr(pos, colon - pos);
      std::string value = trame.substr(colon + 1, comma - colon - 1);

      result[key] = value;
    }

    pos = comma + 1;
  }

  return result;
}

ParsedData parseJson(JsonObject root) {
  ParsedData result;

  for (auto kv : root) {
    std::string key = kv.key().c_str();

    if (kv.value().is<const char *>()) {
      result[key] = kv.value().as<const char *>();
    } else if (kv.value().is<float>() || kv.value().is<int>()) {
      char buffer[16];
      snprintf(buffer, sizeof(buffer), "%.2f", kv.value().as<float>());
      result[key] = buffer;
    }
  }

  return result;
}

#ifdef USE_MQTT
void EmonTx::publish_discovery_(const std::string &tag) {
  std::string discovery_prefix = "homeassistant/sensor/";
  std::string object_id = this->base_topic_ + "_" + tag;
  std::replace(object_id.begin(), object_id.end(), '/', '_');

  std::string topic = discovery_prefix + object_id + "/config";
  std::string device_class = guess_device_class(tag);
  std::string state_class = guess_state_class(tag);
  std::string unit = guess_unit(tag);
  std::string mac = get_mac_address();
  mac.erase(std::remove(mac.begin(), mac.end(), ':'), mac.end());
  std::string unique_id = mac + "_" + tag;

  std::string payload = "{";
  payload += "\"name\":\"" + tag + "\",";
  payload += "\"state_topic\":\"" + this->base_topic_ + "/" + tag + "\",";
  payload += "\"unique_id\":\"" + unique_id + "\",";
  if (!device_class.empty()) {
    payload += "\"device_class\":\"" + device_class + "\",";
  }
  if (!unit.empty()) {
    payload += "\"unit_of_measurement\":\"" + unit + "\",";
  }
  if (!state_class.empty()) {
    payload += "\"state_class\":\"" + state_class + "\",";
  }
  payload += "\"device\":{";
  payload += "\"identifiers\":[\"" + mac + "\"],";
  payload += "\"name\":\"" + this->name_ + "\",";
  payload += "\"manufacturer\":\"OpenEnergyMonitor\",";
  payload += "\"model\":\"EmonTx\"";
  payload += "}}";

  this->mqtt_->publish(topic, payload, 0, true);
}
void EmonTx::publish_mqtt_(const ParsedData &parsed_data) {
  if (this->mqtt_ == nullptr) {
    return;
  }

  for (const auto &[tag, value] : parsed_data) {
    // autodiscovery
    if (this->mqtt_autodiscovery_) {
      if (this->discovered_tags_.insert(tag).second) {
        this->publish_discovery_(tag);
      }
    }

    std::string topic = this->base_topic_ + "/" + tag;
    this->mqtt_->publish(topic, value);
  }
}
void EmonTx::publish_mqtt_(JsonObject root) {
  auto parsed = parseJson(root);
  this->publish_mqtt_(parsed);
}
#else
void EmonTx::publish_mqtt_(const ParsedData &parsed_data) {}
void EmonTx::publish_mqtt_(JsonObject root) {}
#endif

#ifdef USE_SENSOR
void EmonTx::update_sensors_(JsonObject root) {
  for (auto &sensor_pair : this->sensors_) {
    const char *tag = sensor_pair.first;
    sensor::Sensor *sensor_ptr = sensor_pair.second;

    if (root[tag].is<JsonVariant>()) {
      float value = root[tag];
      ESP_LOGV(TAG, "Updating sensor '%s' with value: %.2f", tag, value);
      sensor_ptr->publish_state(value);
    }
  }
}
void EmonTx::update_sensors_(const ParsedData &parsed_data) {
  for (auto &sensor_pair : this->sensors_) {
    const char *tag = sensor_pair.first;

    auto it = parsed_data.find(tag);
    if (it == parsed_data.end()) {
      continue;
    }

    const std::string &str_value = it->second;

    bool is_valid = !str_value.empty();
    for (char c : str_value) {
      if (!isdigit(c) && c != '-' && c != '.') {
        is_valid = false;
        break;
      }
    }

    if (!is_valid) {
      ESP_LOGE(TAG, "Failed to convert value for sensor '%s'", tag);
      continue;
    }

    float value = std::stof(str_value);
    sensor_pair.second->publish_state(value);
  }
}
#else
void EmonTx::update_sensors_(JsonObject root) {}
void EmonTx::update_sensors_(const ParsedData &) {}
#endif

void EmonTx::setup() {
  this->state_ = ParseState::OFF;

  // Pre-allocate buffer to maximum size to prevent reallocation overhead
  // during JSON message collection.
  // 198 bytes should be enough to contain a full session in historical mode with
  // three phases. But go with 1024 just to be sure.
  this->buffer_.reserve(1024);
}

void EmonTx::update() {
  ESP_LOGD(TAG, "Updating EmonTx state...");

  if (this->state_ == ParseState::OFF) {
    this->buffer_.clear();
    this->state_ = ParseState::WAITING_FOR_START;
    ESP_LOGD(TAG, "EmonTx activated and ready to receive data.");
  } else {
    ESP_LOGV(TAG, "EmonTx already active (state: %d)", static_cast<int>(this->state_));
  }
}

bool EmonTx::parse_and_process_json_(const std::string &line) {
  return json::parse_json(line, [this, &line](JsonObject root) {
    this->update_sensors_(root);
    this->publish_mqtt_(root);
    return true;
  });
}

void EmonTx::process_line_(const std::string &line) {
  if (line.empty()) {
    return;
  }

  ESP_LOGD(TAG, "Received line: %s", line.c_str());

  if (line[0] == '{') {
    ESP_LOGV(TAG, "Line is JSON");
    if (!this->parse_and_process_json_(line)) {
      ESP_LOGW(TAG, "Failed to parse JSON");
    }
    return;
  }

  ESP_LOGV(TAG, "Line is plain text");
  auto parsed_data = parseTrame(line);
  this->update_sensors_(parsed_data);
  this->publish_mqtt_(parsed_data);
}

void EmonTx::handle_complete_line_() {
  if (this->buffer_.empty()) {
    return;
  }

  static std::string line = []() {
    std::string s;
    s.reserve(1024);
    return s;
  }();

  line.swap(this->buffer_);
  this->buffer_.clear();

  this->process_line_(line);
}

void EmonTx::append_to_buffer_(char c) {
  if (this->buffer_.length() >= 1024) {
    ESP_LOGW(TAG, "Buffer overflow (>1024 bytes), discarding buffer");
    this->buffer_.clear();
    return;
  }

  this->buffer_ += c;
}

void EmonTx::loop() {
  if (this->state_ == ParseState::OFF) {
    return;
  }

  while (this->available() > 0) {
    char c = static_cast<char>(this->read());

    if (c == '\r') {
      continue;
    }

    if (c == '\n') {
      this->handle_complete_line_();
      continue;
    }

    this->append_to_buffer_(c);
  }
}

#ifdef USE_SENSOR
void EmonTx::dump_sensors_config_() {
  ESP_LOGCONFIG(TAG, "  Registered sensors: %zu", this->sensors_.size());
  for (const auto &sensor_pair : this->sensors_) {
    ESP_LOGCONFIG(TAG, "    Sensor: %s", sensor_pair.first);
  }
}
#else
void EmonTx::dump_sensors_config_() { ESP_LOGCONFIG(TAG, "  Sensor support: DISABLED"); }
#endif

void EmonTx::dump_config() {
  ESP_LOGCONFIG(TAG, "EmonTx:");
  this->dump_sensors_config_();
}

#ifdef USE_SENSOR
void EmonTx::register_sensor(const char *tag_name, sensor::Sensor *sensor) {
  ESP_LOGCONFIG(TAG, "Registering sensor for tag: %s", tag_name);
  this->sensors_.emplace_back(tag_name, sensor);
}
#endif

}  // namespace esphome::emontx
