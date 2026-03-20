#pragma once

#include <map>
#include <vector>
#include <string>
#include <set>

#include "esphome/core/component.h"
#include "esphome/core/defines.h"
#include "esphome/core/helpers.h"
#include "esphome/components/uart/uart.h"
#include "esphome/components/json/json_util.h"
#ifdef USE_SENSOR
#include "esphome/components/sensor/sensor.h"
#endif
#ifdef USE_MQTT
#include "esphome/components/mqtt/mqtt_client.h"
#endif

namespace esphome::emontx {

using ParsedData = std::map<std::string, std::string>;

/**
 * @class EmonTx
 * @brief Main class for the EmonTx component.
 *
 * The EmonTx processes incoming data frames via UART,
 * extracts tags and values, and publishes them to registered sensors.
 */
class EmonTx : public PollingComponent,
               public uart::UARTDevice

{
 public:
  EmonTx() = default;

  void loop() override;
  void setup() override;
  void update() override;
  void dump_config() override;

  void set_base_topic(const std::string &topic) { this->base_topic_ = topic; }
  void set_name(const std::string &name) { this->name_ = name; }

#ifdef USE_SENSOR
  /// @brief Register a sensor to receive updates for a specific JSON tag.
  /// @param tag_name JSON key (string literal).
  /// @param sensor Pointer to the sensor
  void register_sensor(const char *tag_name, sensor::Sensor *sensor);
#endif

#ifdef USE_MQTT
  void set_mqtt_client(mqtt::MQTTClientComponent *mqtt) { this->mqtt_ = mqtt; }
  void set_mqtt_autodiscovery(bool enabled) { this->mqtt_autodiscovery_ = enabled; }
#endif

 protected:
  void update_sensors_(JsonObject root);
  void update_sensors_(const ParsedData &parsed_data);
  bool parse_and_process_json_(const std::string &line);
  void process_line_(const std::string &line);
  void handle_complete_line_();
  void append_to_buffer_(char c);
  void dump_sensors_config_();
  void publish_mqtt_(const ParsedData &parsed_data);
  void publish_mqtt_(JsonObject root);
  std::string buffer_;
  std::set<std::string> discovered_tags_;
  bool mqtt_autodiscovery_{false};
  std::string name_{"emontx"};
  std::string base_topic_{"emon/emontx"};

#ifdef USE_SENSOR
  std::vector<std::pair<const char *, sensor::Sensor *>> sensors_{};
#endif
#ifdef USE_MQTT
  void publish_discovery_(const std::string &tag);
  mqtt::MQTTClientComponent *mqtt_{nullptr};
#endif

  enum class ParseState {
    OFF,
    WAITING_FOR_START,
  };
  ParseState state_{ParseState::OFF};
};

}  // namespace esphome::emontx
