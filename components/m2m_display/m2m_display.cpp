#include "m2m_display.h"

#ifdef USE_ESP32

#include "esphome/core/log.h"
#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"

#include <cstring>
#include <cstdlib>
#include <algorithm>
#include <esp_gap_ble_api.h>

namespace esphome {
namespace m2m_display {

static const char *const TAG = "m2m_display";

static const espbt::ESPBTUUID SERVICE_UUID = espbt::ESPBTUUID::from_uint16(0xFFF0);
static const espbt::ESPBTUUID READ_UUID = espbt::ESPBTUUID::from_uint16(0xFFF2);   // кадр экрана
static const espbt::ESPBTUUID WRITE_UUID = espbt::ESPBTUUID::from_uint16(0xFFF3);  // кнопки / keep-alive

static const uint32_t DISCOVERY_WINDOW_MS = 15000;  // сколько слушать эфир перед выбором счётчика

// Экраны, где долгое «вправо» открывает подменю только для просмотра (Приложение Г).
static const char *const SAFE_HOSTS[] = {"А.ЭНЕРГИЯ+ОБЩ.", "А.ЭНЕРГИЯ-ОБЩ.", "Р.ЭНЕРГИЯ+ОБЩ.", "Р.ЭНЕРГИЯ-ОБЩ.",
                                         "А.ЭНЕРГИЯ+МЕС.", "А.ЭНЕРГИЯ-МЕС.", "Р.ЭНЕРГИЯ+МЕС.", "Р.ЭНЕРГИЯ-МЕС.",
                                         "МАКС.МОЩНОСТИМЕС.", "ПАРАМЕТРЫНАГР."};

// ---------------------------------------------------------------------------------------------
// настройка

void M2MDisplay::add_entry(const char *key, const char *title, uint8_t match, const char *host, uint8_t field,
                           const char *arg, bool live, uint16_t every, sensor::Sensor *s) {
  Entry e;
  e.key = key;
  e.title = match == M_ID ? std::string(title) : norm_(title);
  e.host = (host[0] == 0 || host[0] == '#' || strcmp(host, "main") == 0) ? std::string(host) : norm_(host);
  e.arg = norm_(arg);
  e.match = match;
  e.field = field;
  e.live = live;
  e.every = every ? every : 1;
  e.sens = s;
  this->entries_.push_back(e);
}

void M2MDisplay::add_text_entry(const char *key, const char *title, uint8_t match, const char *host,
                                uint8_t field, const char *arg, bool live, uint16_t every,
                                text_sensor::TextSensor *s) {
  this->add_entry(key, title, match, host, field, arg, live, every, nullptr);
  this->entries_.back().tsens = s;
}

void M2MDisplay::setup() {}

void M2MDisplay::dump_config() {
  ESP_LOGCONFIG(TAG, "M2M display (BLE):");
  ESP_LOGCONFIG(TAG, "  Read interval: %" PRIu32 " ms", this->read_interval_ms_);
  ESP_LOGCONFIG(TAG, "  Keep-alive interval: %" PRIu32 " ms", this->keepalive_interval_ms_);
  ESP_LOGCONFIG(TAG, "  Screen timeout: %" PRIu32 " ms", this->screen_timeout_ms_);
  ESP_LOGCONFIG(TAG, "  Log frames: %s, report unknown: %s", YESNO(this->log_frames_), YESNO(this->report_unknown_));
  if (this->parent()->address_str()[0] == '\0')
    ESP_LOGCONFIG(TAG, "  Адрес счётчика не задан — поиск по имени «%s…» или сервису FFF0",
                  this->discovery_name_.c_str());
  std::string keys;
  for (auto &e : this->entries_)
    keys += e.key + (e.every > 1 ? "/" + std::to_string(e.every) : "") + " ";
  ESP_LOGCONFIG(TAG, "  Сенсоры: %s", keys.c_str());
  LOG_UPDATE_INTERVAL(this);
}

// ---------------------------------------------------------------------------------------------
// автоматический поиск счётчика: ble_client с mac_address 00:00:00:00:00:00

#ifdef USE_ESP32_BLE_DEVICE
bool M2MDisplay::parse_device(const espbt::ESPBTDevice &device) {
  if (this->parent()->address_str()[0] != '\0')
    return false;  // адрес задан в настройках или уже найден
  std::string name;
  auto nm = device.get_name();
  if (nm.size() > 0)
    name.assign(nm.c_str(), nm.size());
  bool ours = !this->discovery_name_.empty() && name.rfind(this->discovery_name_, 0) == 0;
  for (auto &u : device.get_service_uuids())
    if (u == SERVICE_UUID)
      ours = true;
  if (!ours)
    return false;
  uint64_t addr = device.address_uint64();
  for (auto &f : this->found_) {
    if (f.addr == addr) {
      f.rssi = device.get_rssi();
      if (!name.empty())
        f.name = name;
      return false;
    }
  }
  if (this->found_.size() >= 8)
    return false;
  this->found_.push_back(Found{addr, name, device.get_rssi()});
  if (!this->discovering_) {
    this->discovering_ = true;
    this->discovery_ms_ = millis();
  }
  char mac[18];
  snprintf(mac, sizeof(mac), "%02X:%02X:%02X:%02X:%02X:%02X", (uint8_t) (addr >> 40), (uint8_t) (addr >> 32),
           (uint8_t) (addr >> 24), (uint8_t) (addr >> 16), (uint8_t) (addr >> 8), (uint8_t) addr);
  ESP_LOGI(TAG, "Найден счётчик: «%s» %s (%d дБм)", name.c_str(), mac, device.get_rssi());
  return false;
}
#endif

// ---------------------------------------------------------------------------------------------
// Bluetooth

void M2MDisplay::gattc_event_handler(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if,
                                     esp_ble_gattc_cb_param_t *param) {
  switch (event) {
    case ESP_GATTC_OPEN_EVT:
      if (param->open.status == ESP_GATT_OK) {
        ESP_LOGI(TAG, "Подключено к счётчику");
      }
      break;
    case ESP_GATTC_DISCONNECT_EVT:
      ESP_LOGW(TAG, "Связь со счётчиком потеряна (reason 0x%02X)", param->disconnect.reason);
      this->ready_ = false;
      this->op_in_flight_ = false;
      this->synced_ = false;
      this->idx_ = 0;
      this->read_handle_ = 0;
      this->write_handle_ = 0;
      if (this->cycling_)
        this->finish_cycle_("связь потеряна");
      this->same_count_ = 0;
      break;
    case ESP_GATTC_SEARCH_CMPL_EVT: {
      auto *rd = this->parent()->get_characteristic(SERVICE_UUID, READ_UUID);
      auto *wr = this->parent()->get_characteristic(SERVICE_UUID, WRITE_UUID);
      if (rd == nullptr || wr == nullptr) {
        ESP_LOGE(TAG, "Не найдены характеристики FFF2/FFF3 в сервисе FFF0 — это не счётчик М2М с экраном по BLE?");
        break;
      }
      this->read_handle_ = rd->handle;
      this->write_handle_ = wr->handle;
      this->ready_ = true;
      this->op_in_flight_ = false;
      this->node_state = espbt::ClientState::ESTABLISHED;
      if (this->cycle_no_ == 0)
        this->first_update_ms_ = millis() + 60000 + 1;  // первый обход — через минуту после подключения
      this->last_write_ms_ = millis();
      this->last_change_ms_ = millis();
      {
        // просим спокойный интервал соединения: при частом (7.5 мс) Bluetooth-модуль счётчика
        // перестаёт обновлять картинку экрана (проверено на ESP32-S3)
        esp_ble_conn_update_params_t cp{};
        memcpy(cp.bda, this->parent()->get_remote_bda(), sizeof(esp_bd_addr_t));
        cp.min_int = 0x28;  // 50 мс
        cp.max_int = 0x50;  // 100 мс
        cp.latency = 0;
        cp.timeout = 500;  // 5 с
        esp_ble_gap_update_conn_params(&cp);
      }
      ESP_LOGI(TAG, "Готово: FFF2 (экран) handle 0x%04X, FFF3 (кнопки) handle 0x%04X", this->read_handle_,
               this->write_handle_);
      break;
    }
    case ESP_GATTC_READ_CHAR_EVT: {
      if (param->read.conn_id != this->parent()->get_conn_id() || param->read.handle != this->read_handle_)
        break;
      this->op_in_flight_ = false;
      if (param->read.status != ESP_GATT_OK) {
        ESP_LOGW(TAG, "Ошибка чтения FFF2: status %d", param->read.status);
        break;
      }
      this->on_chunk_(param->read.value, param->read.value_len);
      break;
    }
    case ESP_GATTC_WRITE_CHAR_EVT: {
      if (param->write.conn_id != this->parent()->get_conn_id() || param->write.handle != this->write_handle_)
        break;
      this->op_in_flight_ = false;
      if (param->write.status != ESP_GATT_OK) {
        ESP_LOGW(TAG, "Ошибка записи FFF3: status %d", param->write.status);
      }
      break;
    }
    default:
      break;
  }
}

void M2MDisplay::issue_read_() {
  auto err = esp_ble_gattc_read_char(this->parent()->get_gattc_if(), this->parent()->get_conn_id(),
                                     this->read_handle_, ESP_GATT_AUTH_REQ_NONE);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "read_char error %d", err);
    return;
  }
  this->op_in_flight_ = true;
  this->op_started_ms_ = millis();
  this->last_read_ms_ = millis();
}

void M2MDisplay::issue_write_(uint8_t value) {
  uint8_t v = value;
  auto err = esp_ble_gattc_write_char(this->parent()->get_gattc_if(), this->parent()->get_conn_id(),
                                      this->write_handle_, 1, &v, ESP_GATT_WRITE_TYPE_RSP, ESP_GATT_AUTH_REQ_NONE);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "write_char error %d", err);
    return;
  }
  this->op_in_flight_ = true;
  this->op_started_ms_ = millis();
  this->last_write_ms_ = millis();
  if (value != 0xFF)
    ESP_LOGI(TAG, "Кнопка: %u", value);
}

void M2MDisplay::loop() {
  uint32_t now = millis();

  // автоматический поиск: выбираем счётчик, когда он один
  if (this->discovering_ && this->parent()->address_str()[0] == '\0' &&
      now - this->discovery_ms_ > DISCOVERY_WINDOW_MS) {
    std::string list;
    for (auto &f : this->found_) {
      char mac[18];
      snprintf(mac, sizeof(mac), "%02X:%02X:%02X:%02X:%02X:%02X", (uint8_t) (f.addr >> 40),
               (uint8_t) (f.addr >> 32), (uint8_t) (f.addr >> 24), (uint8_t) (f.addr >> 16), (uint8_t) (f.addr >> 8),
               (uint8_t) f.addr);
      if (!list.empty())
        list += "; ";
      list += (f.name.empty() ? std::string("?") : f.name) + " " + mac + " (" + std::to_string(f.rssi) + " дБм)";
    }
    if (this->found_.size() == 1) {
      ESP_LOGI(TAG, "Подключаюсь к найденному счётчику: %s", list.c_str());
      this->parent()->set_address(this->found_[0].addr);
      this->discovering_ = false;
    } else if (now - this->found_log_ms_ > 60000) {
      this->found_log_ms_ = now;
      ESP_LOGW(TAG, "Рядом несколько счётчиков: %s. Укажите нужный в ble_client: mac_address", list.c_str());
    }
    if (this->found_sensor_ != nullptr && this->found_sensor_->state != list)
      this->found_sensor_->publish_state(list);
  }

  if (this->live_ && (int32_t) (now - this->live_until_ms_) > 0) {
    ESP_LOGI(TAG, "Живой экран выключен: долго не было нажатий");
    this->set_live(false);
  }

  if (!this->ready_)
    return;

  if (this->first_update_ms_ != 0 && (int32_t) (now - this->first_update_ms_) > 0) {
    this->first_update_ms_ = 0;
    ESP_LOGI(TAG, "Обход экранов по расписанию «через минуту»");
    this->update();
    return;  // `now` уже устарело относительно cycle_ms_ — проверки сторожа на следующем loop()
  }

  // сторож: на начальном экране время меняется каждую минуту; если картинка не меняется 3 минуты — она «застыла»
  if (this->have_frame_ && now - this->last_change_ms_ > 180000) {
    ESP_LOGW(TAG, "Картинка экрана не меняется 3 минуты — переподключаю Bluetooth");
    this->last_change_ms_ = now;
    this->stale_count_++;
    if (this->cycling_)
      this->finish_cycle_("картинка застыла");
    this->parent()->disconnect();
    return;
  }

  if (this->cycling_) {
    if (now - this->cycle_ms_ > 420000) {
      this->finish_cycle_("обход идёт дольше 7 минут");
    } else if (!this->moved_ && now - this->step_ms_ > this->screen_timeout_ms_) {
      // экран не сменился: первое нажатие счётчик тратит на «проснуться», либо нажатие потерялось
      this->steps_++;
      if (this->steps_ > 90) {
        this->finish_cycle_("слишком много шагов");
      } else if (this->last_press_ == 4 &&
                 (this->last_key_ != this->pressed_from_ || !this->long_right_allowed_(this->last_decoded_))) {
        // долгое «вправо» повторяем только на том же безопасном экране
        ESP_LOGD(TAG, "Долгое «вправо» не повторяю: экран «%s»", this->last_key_.c_str());
        this->moved_ = true;
        this->step_ms_ = now;
      } else {
        ESP_LOGD(TAG, "Экран «%s» не сменился, нажимаю ещё раз", this->pressed_from_.c_str());
        this->presses_++;
        this->push_(this->last_press_);
        this->step_ms_ = now;
      }
    } else if (this->moved_ && now - this->step_ms_ > 3 * this->screen_timeout_ms_) {
      this->finish_cycle_("экран не читается");
    }
  }

  if (this->op_in_flight_) {
    if (now - this->op_started_ms_ > 3000) {
      ESP_LOGW(TAG, "Нет ответа на операцию 3 с, продолжаю");
      this->op_in_flight_ = false;
    }
    return;
  }

  if (!this->pending_writes_.empty()) {
    uint8_t code = this->pending_writes_.front();
    this->pending_writes_.pop_front();
    this->issue_write_(code);
    return;
  }
  if (now - this->last_write_ms_ >= this->keepalive_interval_ms_) {
    this->issue_write_(0xFF);
    return;
  }
  if (now - this->last_read_ms_ >= this->read_interval_ms_) {
    this->issue_read_();
  }

  if (now - this->last_stats_ms_ > 300000) {
    this->last_stats_ms_ = now;
    ESP_LOGD(TAG, "Статистика: чтений %" PRIu32 ", байт %" PRIu32 ", кадров %" PRIu32 ", застываний %" PRIu32,
             this->reads_, this->bytes_, this->frames_, this->stale_count_);
  }
}

void M2MDisplay::on_chunk_(const uint8_t *data, uint16_t len) {
  this->reads_++;
  this->bytes_ += len;
  if (len == 1 && data[0] == 0) {
    // маркер начала кадра (как в приложении)
    this->markers_++;
    this->idx_ = 0;
    this->synced_ = true;
    return;
  }
  for (uint16_t i = 0; i < len; i++) {
    if (this->idx_ < FRAME_SIZE)
      this->buf_[this->idx_] = data[i];
    this->idx_++;
  }
  if (this->idx_ == FRAME_SIZE || (this->idx_ > FRAME_SIZE && this->idx_ - len < FRAME_SIZE)) {
    if (this->synced_)
      this->on_frame_();
  }
}

void M2MDisplay::on_frame_() {
  this->frames_++;
  bool changed = !this->have_frame_ || memcmp(this->buf_, this->last_frame_, FRAME_SIZE) != 0;
  memcpy(this->last_frame_, this->buf_, FRAME_SIZE);
  this->have_frame_ = true;
  if (changed) {
    this->frame_changed_since_log_ = true;
    this->last_change_ms_ = millis();
  }

  ScreenData d = this->decoder_.decode(this->buf_);
  if (d == this->last_decoded_) {
    if (this->same_count_ < 255)
      this->same_count_++;
  } else {
    this->same_count_ = 1;
    this->last_decoded_ = d;
  }
  std::string key = this->key_of_(d);
  if (key == this->last_key_) {
    if (this->key_count_ < 255)
      this->key_count_++;
  } else {
    this->key_count_ = 1;
    this->last_key_ = key;
    this->visit_++;
  }

  this->read_values_(d);
  if (this->live_)
    this->publish_live_(d, this->live_force_);
  if (this->key_count_ == 2)
    this->report_(d, key);
  if (this->cycling_)
    this->cycle_step_(d, key);

  uint32_t now = millis();
  if (this->log_frames_ && this->frame_changed_since_log_ && now - this->last_log_ms_ >= this->log_min_interval_ms_) {
    this->last_log_ms_ = now;
    this->frame_changed_since_log_ = false;
    this->log_frame_();
  }
}

// ---------------------------------------------------------------------------------------------
// разбор экрана

// Заголовок без номера экрана в начале: «2.А.ЭНЕРГИЯ+ОБЩ.» -> «А.ЭНЕРГИЯ+ОБЩ.»
std::string M2MDisplay::bare_title_(const std::string &title) {
  size_t i = 0;
  while (i < title.size() && ((title[i] >= '0' && title[i] <= '9') || title[i] == '.'))
    i++;
  return title.substr(i);
}

// Латинские буквы, неотличимые на экране от русских, приводим к русским — для сравнения.
std::string M2MDisplay::norm_(const std::string &s) {
  struct P {
    char c;
    const char *r;
  };
  static const P PAIRS[] = {{'A', "А"}, {'B', "В"}, {'C', "С"}, {'E', "Е"}, {'H', "Н"}, {'K', "К"},
                            {'M', "М"}, {'O', "О"}, {'P', "Р"}, {'T', "Т"}, {'X', "Х"}, {'Y', "У"},
                            {'a', "а"}, {'c', "с"}, {'e', "е"}, {'o', "о"}, {'p', "р"}, {'x', "х"},
                            {'y', "у"}, {'k', "к"}};
  std::string out;
  out.reserve(s.size() * 2);
  for (char ch : s) {
    const char *r = nullptr;
    for (const auto &p : PAIRS)
      if (p.c == ch)
        r = p.r;
    if (r)
      out += r;
    else
      out += ch;
  }
  return out;
}

// Номер экрана верхнего уровня (2, 3, …) или -1 (начальный экран, подменю, 1.x)
int M2MDisplay::screen_no_(const ScreenData &s) const {
  if (s.id.empty() || s.id.find('.') != std::string::npos)
    return -1;
  int n = atoi(s.id.c_str());
  return (n >= 2 && n <= 31) ? n : -1;
}

// Ключ экрана для навигации: номер («2», «1.5»), «main», «t:<заголовок>» для подменю или «unknown»
std::string M2MDisplay::key_of_(const ScreenData &s) const {
  if (!s.id.empty())
    return s.id;
  if (s.title.rfind("ТЕКУЩИЙ", 0) == 0)
    return "main";
  if (!s.title.empty() && s.title.find('?') == std::string::npos)
    return "t:" + s.title;
  // запасной вариант для начального экрана: часы — 4 крупные и 2 мелкие цифры
  if (s.ok && s.big.size() == 4 && s.small.size() == 2 && !s.text.empty())
    return "main";
  return "unknown";
}

bool M2MDisplay::matches_(const Entry &e, const ScreenData &s, const std::string &bare) const {
  switch (e.match) {
    case M_ID:
      return s.id == e.title;
    case M_PREFIX:
      return bare.compare(0, e.title.size(), e.title) == 0;
    default:
      return bare == e.title;
  }
}

bool M2MDisplay::long_right_allowed_(const ScreenData &s) const {
  std::string key = this->key_of_(s);
  if (key == "main" || key == "1.6")
    return true;
  if (this->screen_no_(s) <= 0 || s.title.find('?') != std::string::npos)
    return false;
  std::string bare = norm_(bare_title_(s.title));
  for (const char *t : SAFE_HOSTS)
    if (bare == t)
      return true;
  return false;
}

bool M2MDisplay::press_long_right_safe() {
  if (!this->long_right_allowed_(this->last_decoded_) || this->key_count_ < 2) {
    ESP_LOGW(TAG, "Долгое «вправо» не нажато: экран «%s» не в списке безопасных",
             this->last_decoded_.title_sp.c_str());
    return false;
  }
  this->pending_writes_.push_back(4);
  return true;
}

// число в строке s начиная с позиции from: «-12,5» -> -12.5; возвращает позицию после числа или npos
static size_t find_number(const std::string &s, size_t from, float *out) {
  for (size_t i = from; i < s.size(); i++) {
    bool neg = s[i] == '-' && i + 1 < s.size() && s[i + 1] >= '0' && s[i + 1] <= '9';
    if (!(s[i] >= '0' && s[i] <= '9') && !neg)
      continue;
    std::string num;
    size_t j = i;
    if (neg) {
      num += '-';
      j++;
    }
    bool dot = false;
    while (j < s.size()) {
      char c = s[j];
      if (c >= '0' && c <= '9') {
        num += c;
      } else if ((c == '.' || c == ',') && !dot && j + 1 < s.size() && s[j + 1] >= '0' && s[j + 1] <= '9') {
        num += '.';
        dot = true;
      } else {
        break;
      }
      j++;
    }
    *out = strtof(num.c_str(), nullptr);
    return j;
  }
  return std::string::npos;
}

// поиск слова-ключа без учёта пробелов: «ДИФФ.ТОК» находится в «ДИФФ. ТОК 40мА»; возвращает позицию после ключа
static size_t find_key(const std::string &text, const std::string &key) {
  for (size_t i = 0; i < text.size(); i++) {
    size_t a = i, b = 0;
    while (a < text.size() && b < key.size()) {
      if (text[a] == ' ' && key[b] != ' ') {
        a++;
        continue;
      }
      if (text[a] != key[b])
        break;
      a++;
      b++;
    }
    if (b == key.size())
      return a;
  }
  return std::string::npos;
}

bool M2MDisplay::parse_(const Entry &e, const ScreenData &s, float *num, std::string *txt) const {
  std::string joined;  // весь текст экрана: заголовок и строки тела
  joined = s.title_sp;
  for (auto &l : s.lines)
    joined += " " + l.text;
  switch (e.field) {
    case F_VALUE: {
      if (!s.ok || s.big.empty())
        return false;
      std::string v = s.big;
      if (!s.small.empty())
        v += "." + s.small;
      *num = strtof(v.c_str(), nullptr);
      return true;
    }
    case F_TEXTNUM: {
      for (auto &l : s.lines)
        if (find_number(l.text, 0, num) != std::string::npos)
          return true;
      if (!s.text.empty()) {
        *num = strtof(s.text.c_str(), nullptr);
        return true;
      }
      return false;
    }
    case F_PHASE: {
      for (auto &l : s.lines) {
        std::string t = norm_(l.text);
        size_t p = 0;
        while (p < t.size() && t[p] == ' ')
          p++;
        if (t.compare(p, e.arg.size(), e.arg) != 0)
          continue;
        size_t q = p + e.arg.size();
        if (q < t.size() && t[q] != ' ' && !(t[q] >= '0' && t[q] <= '9') && t[q] != ':' && t[q] != '=')
          continue;  // не отдельная буква фазы
        if (find_number(t, q, num) != std::string::npos)
          return true;
      }
      return false;
    }
    case F_KEYNUM: {
      std::string t = norm_(joined);
      size_t p = find_key(t, e.arg);
      return p != std::string::npos && find_number(t, p, num) != std::string::npos;
    }
    case F_KEYTEXT: {
      size_t p = find_key(norm_(joined), e.arg);
      if (p == std::string::npos)
        return false;
      // позиция в нормализованной строке; берём слово из неё же (латиница -> кириллица не важна для цифр)
      std::string t = norm_(joined);
      while (p < t.size() && t[p] == ' ')
        p++;
      size_t q = p;
      while (q < t.size() && t[q] != ' ')
        q++;
      if (q == p)
        return false;
      *txt = t.substr(p, q - p);
      return txt->find('?') == std::string::npos;
    }
    case F_TEXT: {
      std::string t;
      for (auto &l : s.lines) {
        if (!t.empty())
          t += " / ";
        t += l.text;
      }
      if (t.empty())
        return false;
      *txt = t;
      return true;
    }
    case F_TARIFF: {
      size_t p = std::string("ТЕКУЩИЙТ").size();
      std::string t = norm_(s.title);
      if (t.rfind("ТЕКУЩИЙТ", 0) != 0 || p >= t.size() || t[p] < '1' || t[p] > '8')
        return false;
      *txt = std::string("T") + t[p];
      return true;
    }
  }
  return false;
}

void M2MDisplay::read_values_(const ScreenData &s) {
  std::string bare = norm_(bare_title_(s.title));
  for (auto &e : this->entries_) {
    // тариф читаем на каждом кадре начального экрана (ESP стоит на нём почти всё время) —
    // смена Т1/Т2 видна сразу, а не только при обходе
    bool tariff = e.field == F_TARIFF;
    if ((!tariff && e.visit == this->visit_) || !this->matches_(e, s, bare))
      continue;
    bool stable = this->same_count_ >= 2 || (e.live && this->key_count_ >= 2);
    if (!stable)
      continue;
    float v = NAN;
    std::string t;
    if (!this->parse_(e, s, &v, &t))
      continue;
    if (tariff && e.visit == this->visit_) {
      // уже опубликован в этом посещении: публикуем только смену, подтверждённую 3 кадрами подряд
      if (e.tsens == nullptr || e.tsens->state == t) {
        this->tariff_cand_n_ = 0;
        continue;
      }
      if (t != this->tariff_cand_) {
        this->tariff_cand_ = t;
        this->tariff_cand_n_ = 1;
        continue;
      }
      if (++this->tariff_cand_n_ < 3)
        continue;
      ESP_LOGI(TAG, "Смена тарифа: %s -> %s", e.tsens->state.c_str(), t.c_str());
    }
    if (e.key == "energy_import_total") {
      // защита от ошибки распознавания: показание не убывает и не прыгает больше чем на 50 кВт*ч
      if (!std::isnan(this->last_total_) && (v < this->last_total_ - 0.005f || v > this->last_total_ + 50.0f)) {
        ESP_LOGW(TAG, "Показание %.2f отброшено (предыдущее %.2f)", v, this->last_total_);
        e.visit = this->visit_;
        continue;
      }
      this->last_total_ = v;
    }
    e.visit = this->visit_;
    e.need = false;
    if (e.sens != nullptr) {
      ESP_LOGD(TAG, "%s = %g (экран «%s»)", e.key.c_str(), v, s.title_sp.c_str());
      e.sens->publish_state(v);
    } else if (e.tsens != nullptr) {
      ESP_LOGD(TAG, "%s = «%s» (экран «%s»)", e.key.c_str(), t.c_str(), s.title_sp.c_str());
      if (e.tsens->state != t || !e.tsens->has_state())
        e.tsens->publish_state(t);
    }
  }
}

// Отчёт для разработки: описание каждого нового экрана и незнакомые знаки (их можно прислать в issue).
void M2MDisplay::report_(const ScreenData &s, const std::string &key) {
  if (!this->report_unknown_)
    return;
  auto fnv = [](const std::string &x) {
    uint32_t h = 0x811C9DC5u;
    for (char c : x)
      h = (h ^ (uint8_t) c) * 0x01000193u;
    return h;
  };
  if (this->reported_.size() < 200) {
    std::string desc = key.rfind("t:", 0) == 0 ? "t:" + norm_(bare_title_(s.title)) : key;
    uint32_t h = fnv("screen:" + desc);
    if (key != "main" && !this->reported_.count(h)) {
      this->reported_.insert(h);
      std::string lines;
      for (auto &l : s.lines)
        lines += " [" + l.text + "]";
      int n = 0;
      std::string bare = norm_(bare_title_(s.title));
      for (auto &e : this->entries_)
        if (this->matches_(e, s, bare))
          n++;
      ESP_LOGI(TAG, "Экран «%s»: значение «%s,%s», текст:%s; сенсоров для него: %d", s.title_sp.c_str(),
               s.big.c_str(), s.small.c_str(), lines.empty() ? " —" : lines.c_str(), n);
    }
  }
  for (auto &u : s.unknown) {
    std::string k((const char *) u.cols, u.w);
    uint32_t h = fnv("glyph:" + k);
    if (this->reported_.count(h) || this->reported_.size() >= 200)
      continue;
    this->reported_.insert(h);
    char hex[8 * 3 + 1] = {0};
    for (int i = 0; i < u.w && i < 8; i++)
      sprintf(hex + i * 3, "%02X ", u.cols[i]);
    char rows[7][10];
    for (int r = 0; r < 7; r++) {
      int i = 0;
      for (; i < u.w && i < 8; i++)
        rows[r][i] = (u.cols[i] >> r) & 1 ? '#' : '.';
      rows[r][i] = 0;
    }
    ESP_LOGW(TAG, "Незнакомый знак на экране «%s»: ширина %u, столбцы %s| %s | %s | %s | %s | %s | %s | %s |",
             s.title_sp.c_str(), u.w, hex, rows[0], rows[1], rows[2], rows[3], rows[4], rows[5], rows[6]);
  }
}

// ---------------------------------------------------------------------------------------------
// обход экранов

int M2MDisplay::ring_index_(int n) {
  for (size_t i = 0; i < this->ring_.size(); i++)
    if (this->ring_[i] == n)
      return (int) i + 1;  // 0 — начальный экран
  auto it = this->ring_.begin();
  while (it != this->ring_.end() && *it < n)
    ++it;
  this->ring_.insert(it, (uint8_t) n);
  return this->ring_index_(n);
}

bool M2MDisplay::host_needed_(const std::string &host) const {
  for (auto &e : this->entries_)
    if (e.need && e.host == host)
      return true;
  return false;
}

// Есть ли нужные значения, экран которых ещё не встречался (тогда незнакомые экраны надо посмотреть)
static bool title_matches(const Entry &e, const std::string &t) {
  if (e.match == M_PREFIX)
    return t.compare(0, e.title.size(), e.title) == 0;
  return t == e.title;
}

bool M2MDisplay::screen_wanted_(int n) const {
  auto it = this->titles_.find(n);
  if (it == this->titles_.end()) {
    // экран ещё не видели: нужен, если какое-то значение пока не нашло своего экрана
    for (auto &e : this->entries_) {
      if (!e.need || e.field == F_TARIFF || e.host == "main" || e.host == "#1.6")
        continue;
      if (e.host.empty() && e.match == M_ID) {
        if (atoi(e.title.c_str()) == n)
          return true;
        continue;
      }
      const std::string &t = e.host.empty() ? e.title : e.host;
      bool located = false;
      for (auto &kv : this->titles_)
        if ((e.host.empty() && title_matches(e, kv.second)) || (!e.host.empty() && kv.second == t))
          located = true;
      if (!located)
        return true;
    }
    return false;
  }
  const std::string &t = it->second;
  for (auto &e : this->entries_) {
    if (!e.need)
      continue;
    if (e.host.empty() && e.match == M_ID && atoi(e.title.c_str()) == n)
      return true;
    if (e.host.empty() && e.match != M_ID && e.field != F_TARIFF && title_matches(e, t))
      return true;
    if (!e.host.empty() && e.host == t)
      return true;
  }
  return false;
}

void M2MDisplay::mark_absent_(const std::string &host, const char *why) {
  for (auto &e : this->entries_) {
    if (!e.need || (host != "*" && e.host != host))
      continue;
    e.need = false;
    e.absent = true;
    ESP_LOGW(TAG, "«%s»: экран не найден (%s) — на этом счётчике его, видимо, нет", e.key.c_str(), why);
  }
}

void M2MDisplay::update() {
  if (!this->ready_) {
    ESP_LOGD(TAG, "Обход экранов пропущен: нет связи со счётчиком");
    return;
  }
  if (this->cycling_)
    return;
  if (this->live_) {
    ESP_LOGD(TAG, "Обход экранов отложен: включён живой экран");
    this->resume_after_live_ = true;
    return;
  }
  this->cycle_no_++;
  bool retry_absent = this->cycle_no_ % 24 == 0;  // раз в 24 обхода снова ищем «отсутствующие» экраны
  int cnt = 0;
  for (auto &e : this->entries_) {
    e.need = false;
    if (e.absent && !retry_absent)
      continue;
    if (e.every > 1 && (this->cycle_no_ - 1) % e.every != 0)
      continue;
    e.absent = false;
    e.need = true;
    cnt++;
  }
  if (cnt == 0)
    return;
  this->visited_ = 0;
  this->sub_seen_.clear();
  this->cycling_ = true;
  this->steps_ = 1;
  this->unknown_count_ = 0;
  this->cycle_ms_ = millis();
  this->step_ms_ = millis();
  this->pressed_from_ = this->last_key_;
  this->moved_ = false;
  this->last_screen_ = this->last_decoded_;
  // значения с текущего экрана уже могли прочитаться — сразу решаем, куда идти
  this->visit_++;
  uint8_t code = this->decide_(this->last_key_);
  if (code == 0)
    code = 2;
  ESP_LOGD(TAG, "Начинаю обход экранов №%" PRIu32 " (%d значений, сейчас «%s»)", this->cycle_no_, cnt,
           this->last_key_.c_str());
  this->last_press_ = code;
  this->presses_ = 1;
  this->push_(code);
}

void M2MDisplay::cycle_step_(const ScreenData &s, const std::string &key) {
  if (key == this->pressed_from_ && !this->moved_)
    return;  // ещё старый экран
  if (!this->moved_) {
    this->moved_ = true;
    this->step_ms_ = millis();
  }
  this->last_screen_ = s;
  std::string bare = norm_(bare_title_(s.title));
  bool wanted = false;
  for (auto &e : this->entries_)
    if (e.need && this->matches_(e, s, bare))
      wanted = true;
  bool settled = wanted ? this->key_count_ >= 4 : this->key_count_ >= 2;
  if (!settled)
    return;
  if (wanted) {
    for (auto &e : this->entries_)
      if (e.need && this->matches_(e, s, bare)) {
        ESP_LOGW(TAG, "«%s»: экран «%s» не распознался, пропускаю", e.key.c_str(), s.title_sp.c_str());
        e.need = false;
      }
  }
  int no = this->screen_no_(s);
  if (no > 0) {
    this->ring_index_(no);
    this->visited_ |= 1u << no;
    if (s.title.find('?') == std::string::npos)
      this->titles_[no] = bare;
  }
  int from_no = atoi(this->pressed_from_.c_str());
  bool from_numbered = !this->pressed_from_.empty() && this->pressed_from_[0] >= '0' &&
                       this->pressed_from_[0] <= '9' && this->pressed_from_.find('.') == std::string::npos;
  // «влево» с начального экрана ведёт на последний экран круга — так узнаём, сколько экранов (2…последний).
  // (По «вправо» на начальный экран судить нельзя: счётчик и сам возвращается на него примерно через 1,5 мин.)
  if (this->pressed_from_ == "main" && this->last_press_ == 1 && no > 0 && !this->sweep_done_ &&
      this->presses_ == 1) {
    for (int n = 2; n <= no; n++)
      this->ring_index_(n);
    while (!this->ring_.empty() && this->ring_.back() > no)
      this->ring_.pop_back();
    this->sweep_done_ = true;
  }
  // одно нажатие перевело с экрана a на экран b — номеров между ними на этом счётчике нет
  if (from_numbered && no > 0 && from_no >= 2 && this->presses_ == 1 &&
      (this->last_press_ == 1 || this->last_press_ == 2)) {
    int lo = std::min(from_no, no), hi = std::max(from_no, no);
    for (auto it = this->ring_.begin(); it != this->ring_.end();) {
      if (*it > lo && *it < hi)
        it = this->ring_.erase(it);
      else
        ++it;
    }
  }
  if (this->sweep_done_ && !this->ring_logged_) {
    this->ring_logged_ = true;
    std::string r;
    for (auto n : this->ring_)
      r += std::to_string(n) + " ";
    ESP_LOGI(TAG, "Экраны счётчика: %s", r.c_str());
  }
  if (key == "unknown" && this->log_frames_)
    this->log_frame_();
  uint8_t code = this->decide_(key);
  if (code == 0) {
    this->finish_cycle_(nullptr);
    return;
  }
  this->steps_++;
  if (this->steps_ > 90) {
    this->finish_cycle_("слишком много шагов");
    return;
  }
  this->pressed_from_ = key;
  this->last_press_ = code;
  this->presses_ = 1;
  this->moved_ = false;
  this->step_ms_ = millis();
  this->push_(code);
}

// Какую кнопку нажать, находясь на экране key: 0 — всё прочитано.
uint8_t M2MDisplay::decide_(const std::string &key) {
  const ScreenData &s = this->last_screen_;
  int no = this->screen_no_(s);
  bool on_main = key == "main";
  bool on_1x = s.id.rfind("1.", 0) == 0;
  bool on_sub = key.rfind("t:", 0) == 0 || key == "unknown";
  bool main_needed = this->host_needed_("main") || this->host_needed_("#1.6");

  if (no > 0 || on_main)
    this->sub_host_.clear();  // верхний уровень
  else if (on_1x)
    this->sub_host_ = "main";

  // подменю начального экрана: 1.2 … 1.7 (листаем короткими нажатиями; на 1.7 — только короткие!)
  if (on_1x) {
    if (key == "1.6" && this->host_needed_("#1.6") && this->long_right_allowed_(s)) {
      this->sub_host_ = "#1.6";
      this->sub_steps_ = 0;
      this->sub_seen_["#1.6"].clear();
      return 4;
    }
    if (!main_needed)
      return 3;
    auto &seen = this->sub_seen_["main"];
    if ((this->last_press_ == 2 && seen.count(key)) || this->sub_steps_ > 12) {
      this->mark_absent_("main", "нет в подменю начального экрана");
      this->mark_absent_("#1.6", "нет экрана 1.6");
      return 3;
    }
    seen.insert(key);
    this->sub_steps_++;
    return 2;
  }

  // подменю экрана верхнего уровня
  if (on_sub) {
    if (this->sub_host_.empty())
      return (this->unknown_count_++ % 2 == 0) ? 3 : 2;  // неизвестно, откуда пришли: пробуем выйти, потом листаем
    if (!this->host_needed_(this->sub_host_))
      return 3;
    auto &seen = this->sub_seen_[this->sub_host_];
    if ((this->last_press_ == 2 && seen.count(key)) || this->sub_steps_ > 12) {
      this->mark_absent_(this->sub_host_, "подменю пройдено по кругу");
      return 3;
    }
    seen.insert(key);
    this->sub_steps_++;
    return 2;
  }

  // верхний уровень: открыть подменю, если в нём что-то нужно
  if (on_main && main_needed && this->long_right_allowed_(s)) {
    this->sub_host_ = "main";
    this->sub_steps_ = 0;
    this->sub_seen_["main"].clear();
    return 4;
  }
  if (no > 0) {
    auto it = this->titles_.find(no);
    if (it != this->titles_.end() && this->host_needed_(it->second) && this->long_right_allowed_(s)) {
      this->sub_host_ = it->second;
      this->sub_steps_ = 0;
      this->sub_seen_[it->second].clear();
      return 4;
    }
  }

  // первый обход после включения: доходим до начального экрана и жмём «влево» — узнаём последний экран
  if (!this->sweep_done_)
    return (on_main || key.empty()) ? 1 : 2;

  int pos;
  if (on_main || key.empty())
    pos = 0;
  else if (no > 0)
    pos = this->ring_index_(no);
  else
    return 2;
  bool tariff_needed = false;
  for (auto &e : this->entries_)
    if (e.need && e.field == F_TARIFF)
      tariff_needed = true;
  int size = (int) this->ring_.size() + 1;
  int best = 99;
  uint8_t code = 2;
  auto consider = [&](int tp) {
    if (tp == pos)
      return;
    int dr = (tp - pos + size) % size, dl = (pos - tp + size) % size;
    if (dr < best) {
      best = dr;
      code = 2;
    }
    if (dl < best) {
      best = dl;
      code = 1;
    }
  };
  for (size_t i = 0; i < this->ring_.size(); i++) {
    int n = this->ring_[i];
    bool want = !(this->visited_ & (1u << n)) ? this->screen_wanted_(n) || !this->titles_.count(n)
                                               : this->screen_wanted_(n);
    if (want)
      consider((int) i + 1);
  }
  if (main_needed || tariff_needed)
    consider(0);
  if (best == 99) {
    // всё нужное прочитано или его нет на этом счётчике
    for (auto &e : this->entries_) {
      if (!e.need)
        continue;
      if (on_main && (e.host == "main" || e.host == "#1.6" || e.field == F_TARIFF))
        continue;
      this->mark_absent_(e.host, "экрана нет в круге экранов");
    }
    return 0;
  }
  return code;
}

void M2MDisplay::finish_cycle_(const char *why) {
  this->cycling_ = false;
  this->sub_host_.clear();
  uint32_t secs = (millis() - this->cycle_ms_) / 1000;
  std::string left;
  for (auto &e : this->entries_)
    if (e.need)
      left += e.key + " ";
  if (why == nullptr && left.empty())
    ESP_LOGI(TAG, "Обход экранов завершён: всё прочитано за %" PRIu32 " с, %u шагов", secs, this->steps_);
  else
    ESP_LOGW(TAG, "Обход экранов прерван (%s), не прочитано: %s(%" PRIu32 " с)", why ? why : "-",
             left.empty() ? "— " : left.c_str(), secs);
  for (auto &e : this->entries_)
    e.need = false;
}

// ---------------------------------------------------------------------------------------------
// живой экран

void LiveSwitch::write_state(bool state) {
  if (this->parent_ != nullptr)
    this->parent_->set_live(state);
}

void LiveButton::press_action() {
  if (this->parent_ != nullptr)
    this->parent_->press(this->code_);
}

void M2MDisplay::set_live(bool on) {
  if (on == this->live_) {
    if (this->live_switch_ != nullptr)
      this->live_switch_->publish_state(on);
    if (on)
      this->live_touch_();
    return;
  }
  this->live_ = on;
  if (on) {
    ESP_LOGI(TAG, "Живой экран включён");
    this->live_touch_();
    this->live_force_ = true;
    if (this->cycling_) {
      this->finish_cycle_("включён живой экран");
      this->resume_after_live_ = true;
    }
    if (this->have_frame_)
      this->publish_live_(this->last_decoded_, true);
  } else {
    ESP_LOGI(TAG, "Живой экран выключен");
    if (this->resume_after_live_) {
      // обход был прерван или отложен из-за живого экрана — повторяем через минуту, а не через update_interval
      this->resume_after_live_ = false;
      this->first_update_ms_ = millis() + 60000 + 1;
      ESP_LOGI(TAG, "Прерванный обход повторю через минуту");
    }
  }
  if (this->live_switch_ != nullptr)
    this->live_switch_->publish_state(on);
}

// Кадр — 4 страницы по 160 байт; каждая уходит в свой текстовый сенсор в base64 (216 знаков < 255).
void M2MDisplay::publish_live_(const ScreenData &d, bool force) {
  for (int p = 0; p < 4; p++) {
    const uint8_t *page = this->last_frame_ + p * 160;
    if (!force && memcmp(page, this->live_sent_ + p * 160, 160) == 0)
      continue;
    memcpy(this->live_sent_ + p * 160, page, 160);
    if (this->page_sensors_[p] != nullptr)
      this->page_sensors_[p]->publish_state(base64_encode(page, 160));
  }
  this->live_force_ = false;
  if (this->screen_text_sensor_ != nullptr) {
    std::string t = d.title_sp;
    if (!d.big.empty())
      t += " — " + d.big + (d.small.empty() ? "" : "," + d.small);
    for (auto &l : d.lines)
      t += " · " + l.text;
    if (t.size() > 250) {
      size_t n = 250;
      while (n > 0 && (t[n] & 0xC0) == 0x80)
        n--;  // не резать букву UTF-8 пополам
      t.resize(n);
    }
    if (force || this->screen_text_sensor_->state != t)
      this->screen_text_sensor_->publish_state(t);
  }
}

void M2MDisplay::log_frame_() {
  // HEX — для разбора; 10 строк по 64 байта
  ESP_LOGI(TAG, "FRAME #%" PRIu32 " BEGIN", this->frames_);
  char line[64 * 2 + 1];
  for (int row = 0; row < 10; row++) {
    for (int i = 0; i < 64; i++)
      sprintf(line + i * 2, "%02X", this->last_frame_[row * 64 + i]);
    line[128] = 0;
    ESP_LOGI(TAG, "F%02d %s", row, line);
  }
  // Картинка: экран 160x32, 4 страницы по 160 столбцов, бит 0 — верхняя точка страницы
  char px[161];
  for (int y = 0; y < 32; y++) {
    int page = y / 8, bit = y % 8;
    for (int x = 0; x < 160; x++)
      px[x] = (this->last_frame_[page * 160 + x] >> bit) & 1 ? '#' : '.';
    px[160] = 0;
    ESP_LOGI(TAG, "P%02d %s", y, px);
  }
  ESP_LOGI(TAG, "FRAME END");
}

}  // namespace m2m_display
}  // namespace esphome

#endif
