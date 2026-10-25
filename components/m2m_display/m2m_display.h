#pragma once

#ifdef USE_ESP32

#include "esphome/core/component.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/text_sensor/text_sensor.h"
#include "esphome/components/switch/switch.h"
#include "esphome/components/button/button.h"
#include "esphome/components/ble_client/ble_client.h"
#include "esphome/components/esp32_ble_tracker/esp32_ble_tracker.h"
#include "decoder.h"

#include <esp_gattc_api.h>
#include <deque>
#include <cmath>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace esphome {
namespace m2m_display {

namespace espbt = esphome::esp32_ble_tracker;

static const size_t FRAME_SIZE = 640;  // экран 160 x 32 точки, 4 страницы по 160 байт

// Как достаётся значение с экрана (см. каталог в __init__.py)
enum Field : uint8_t {
  F_VALUE = 0,    // крупные + мелкие цифры: «1234» «56» -> 1234.56
  F_TEXTNUM = 1,  // первое число в строках текста экрана (напряжение, время работы)
  F_PHASE = 2,    // число в строке, начинающейся с буквы фазы (М2М-3Т: «A 1.994kW»)
  F_KEYNUM = 3,   // число после слова-ключа («ДИФФ. ТОК 40мА», «RSSI: 23»)
  F_KEYTEXT = 4,  // слово после ключа («S/N: 0123…»)
  F_TEXT = 5,     // весь текст экрана
  F_TARIFF = 6,   // текущий тариф из заголовка начального экрана
};
enum Match : uint8_t { M_EXACT = 0, M_PREFIX = 1, M_ID = 2 };

struct Entry {
  std::string key;
  std::string title;  // заголовок без пробелов (латиница, похожая на кириллицу, приведена к кириллице)
  std::string host;   // "" верхний уровень, "main", "#1.6" или заголовок экрана-хозяина подменю
  std::string arg;
  uint8_t match;
  uint8_t field;
  bool live;
  uint16_t every;
  sensor::Sensor *sens{nullptr};
  text_sensor::TextSensor *tsens{nullptr};
  bool need{false};    // прочитать в этом обходе
  bool absent{false};  // такого экрана на этом счётчике нет (выяснено обходом)
  uint32_t visit{0};   // номер «посещения» экрана, на котором значение уже опубликовано
};

class M2MDisplay;

// Выключатель «Живой экран»: пока включён, кадр экрана отправляется в HA (для карточки m2m-display-card)
class LiveSwitch : public switch_::Switch {
 public:
  void set_parent(M2MDisplay *p) { this->parent_ = p; }

 protected:
  void write_state(bool state) override;
  M2MDisplay *parent_{nullptr};
};

// Кнопка счётчика для карточки: 1 влево, 2 вправо, 3 долго влево, 4 долго вправо (только безопасные экраны)
class LiveButton : public button::Button {
 public:
  void set_parent(M2MDisplay *p) { this->parent_ = p; }
  void set_code(uint8_t c) { this->code_ = c; }

 protected:
  void press_action() override;
  M2MDisplay *parent_{nullptr};
  uint8_t code_{0};
};

class M2MDisplay : public PollingComponent, public ble_client::BLEClientNode, public espbt::ESPBTDeviceListener {
 public:
  void setup() override;
  void loop() override;
  void update() override;  // начать обход экранов
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

  void gattc_event_handler(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if,
                           esp_ble_gattc_cb_param_t *param) override;
#ifdef USE_ESP32_BLE_DEVICE
  bool parse_device(const espbt::ESPBTDevice &device) override;  // автоматический поиск счётчика
#endif

  void set_read_interval(uint32_t ms) { this->read_interval_ms_ = ms; }
  void set_keepalive_interval(uint32_t ms) { this->keepalive_interval_ms_ = ms; }
  void set_log_frames(bool v) { this->log_frames_ = v; }
  void set_log_min_interval(uint32_t ms) { this->log_min_interval_ms_ = ms; }
  void set_screen_timeout(uint32_t ms) { this->screen_timeout_ms_ = ms; }
  void set_report_unknown(bool v) { this->report_unknown_ = v; }
  void set_discovery_name(const std::string &s) { this->discovery_name_ = s; }
  void set_found_sensor(text_sensor::TextSensor *s) { this->found_sensor_ = s; }
  void add_entry(const char *key, const char *title, uint8_t match, const char *host, uint8_t field,
                 const char *arg, bool live, uint16_t every, sensor::Sensor *s);
  void add_text_entry(const char *key, const char *title, uint8_t match, const char *host, uint8_t field,
                      const char *arg, bool live, uint16_t every, text_sensor::TextSensor *s);

  // Кнопки счётчика: 1 влево, 2 вправо, 3 долго влево (выход из подменю — безопасно),
  // 4 долго вправо — только на экранах, где это открывает подменю просмотра.
  // На экранах 15 (реле нагрузки) и 1.7 (модем, выбор оператора) долгое «вправо» меняет настройки — запрещено.
  void press(uint8_t code) {
    this->live_touch_();
    if (code == 1 || code == 2 || code == 3)
      this->pending_writes_.push_back(code);
    else if (code == 4)
      this->press_long_right_safe();
  }

  // живой экран
  void set_live_switch(LiveSwitch *s) { this->live_switch_ = s; }
  void set_page_sensor(int i, text_sensor::TextSensor *s) { this->page_sensors_[i] = s; }
  void set_screen_text_sensor(text_sensor::TextSensor *s) { this->screen_text_sensor_ = s; }
  void set_live_timeout(uint32_t ms) { this->live_timeout_ms_ = ms; }
  void set_live(bool on);
  bool is_live() const { return this->live_; }
  bool press_long_right_safe();

  uint32_t frames_count() const { return this->frames_; }
  uint32_t stale_count() const { return this->stale_count_; }

 protected:
  void issue_read_();
  void issue_write_(uint8_t value);
  void push_(uint8_t code) { this->pending_writes_.push_back(code); }
  void on_chunk_(const uint8_t *data, uint16_t len);
  void on_frame_();
  void log_frame_();
  void report_(const ScreenData &s, const std::string &key);

  // разбор экрана
  std::string key_of_(const ScreenData &s) const;
  static std::string bare_title_(const std::string &title);
  static std::string norm_(const std::string &s);
  bool matches_(const Entry &e, const ScreenData &s, const std::string &bare) const;
  bool long_right_allowed_(const ScreenData &s) const;
  void read_values_(const ScreenData &s);
  bool parse_(const Entry &e, const ScreenData &s, float *num, std::string *txt) const;
  int screen_no_(const ScreenData &s) const;

  // обход
  void cycle_step_(const ScreenData &s, const std::string &key);
  uint8_t decide_(const std::string &key);
  bool host_needed_(const std::string &host) const;
  bool screen_wanted_(int n) const;
  void mark_absent_(const std::string &host, const char *why);
  void finish_cycle_(const char *why);
  int ring_index_(int n);

  ScreenDecoder decoder_;
  ScreenData last_decoded_;
  uint8_t same_count_{0};
  std::string last_key_;
  ScreenData last_screen_;
  uint8_t key_count_{0};
  uint32_t visit_{0};
  std::string last_logged_;
  std::vector<Entry> entries_;
  float last_total_{NAN};

  // обход экранов
  bool cycling_{false};
  uint32_t cycle_no_{0};
  uint32_t visited_{0};          // бит n — экран верхнего уровня n уже пройден в этом обходе
  std::vector<uint8_t> ring_;    // номера экранов верхнего уровня по кругу (узнаём при первом обходе)
  std::map<int, std::string> titles_;  // номер экрана -> заголовок (без номера)
  bool sweep_done_{false};
  bool ring_logged_{false};
  std::string sub_host_;         // в каком подменю находимся ("" — на верхнем уровне)
  std::map<std::string, std::set<std::string>> sub_seen_;  // какие экраны подменю уже видели
  uint8_t sub_steps_{0};
  uint8_t steps_{0};
  uint32_t cycle_ms_{0};
  uint32_t step_ms_{0};
  std::string pressed_from_;
  uint8_t last_press_{0};
  uint8_t presses_{0};
  bool moved_{false};
  uint8_t unknown_count_{0};
  uint32_t screen_timeout_ms_{9000};

  // отчёт о незнакомом
  bool report_unknown_{true};
  std::set<uint32_t> reported_;

  // автоматический поиск счётчика
  std::string discovery_name_{"M2M"};
  text_sensor::TextSensor *found_sensor_{nullptr};
  struct Found {
    uint64_t addr;
    std::string name;
    int rssi;
  };
  std::vector<Found> found_;
  bool discovering_{false};
  uint32_t discovery_ms_{0};
  uint32_t found_log_ms_{0};
  uint32_t first_update_ms_{0};
  std::string tariff_cand_;
  uint8_t tariff_cand_n_{0};

  uint32_t last_change_ms_{0};
  uint32_t stale_count_{0};

  uint16_t read_handle_{0};
  uint16_t write_handle_{0};
  bool ready_{false};
  bool op_in_flight_{false};
  uint32_t op_started_ms_{0};
  uint32_t last_read_ms_{0};
  uint32_t last_write_ms_{0};
  uint32_t last_log_ms_{0};
  uint32_t last_stats_ms_{0};

  uint32_t read_interval_ms_{150};
  uint32_t keepalive_interval_ms_{8000};
  uint32_t log_min_interval_ms_{5000};
  bool log_frames_{false};

  uint8_t buf_[FRAME_SIZE]{};
  size_t idx_{0};
  bool synced_{false};
  uint8_t last_frame_[FRAME_SIZE]{};
  bool have_frame_{false};
  bool frame_changed_since_log_{false};
  uint32_t frames_{0};
  uint32_t reads_{0};
  uint32_t bytes_{0};
  uint32_t markers_{0};

  std::deque<uint8_t> pending_writes_;

  // живой экран
  void live_touch_() { this->live_until_ms_ = millis() + this->live_timeout_ms_; }
  void publish_live_(const ScreenData &d, bool force);
  LiveSwitch *live_switch_{nullptr};
  text_sensor::TextSensor *page_sensors_[4]{};
  text_sensor::TextSensor *screen_text_sensor_{nullptr};
  uint32_t live_timeout_ms_{180000};
  uint32_t live_until_ms_{0};
  bool live_{false};
  bool live_force_{false};
  uint8_t live_sent_[FRAME_SIZE]{};
};

}  // namespace m2m_display
}  // namespace esphome

#endif
