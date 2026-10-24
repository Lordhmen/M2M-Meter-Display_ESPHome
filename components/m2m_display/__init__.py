"""M2M display: показания счётчиков М2М-1С / М2М-3Т (КПЗ) с экрана по Bluetooth — как приложение «Дисплей М2М».

Протокол (из приложения ru.kpsz.meterdisplay 1.1.1):
  сервис 0xFFF0; 0xFFF2 — чтение кадра экрана (последовательные чтения: [0x00] = начало кадра,
  затем куски, всего 640 байт = 160x32 точки); 0xFFF3 — запись 1 байта «кнопки»:
  1 влево, 2 вправо, 3 долго влево, 4 долго вправо, 0xFF — keep-alive.

Каждый сенсор — один экран (или одна строка экрана) из Приложения Г руководства по эксплуатации.
Экран узнаётся по заголовку буквами, а не по номеру: энергосбыт может скрыть часть экранов.
"""
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import ble_client, esp32_ble_tracker, sensor, text_sensor
from esphome.const import (
    CONF_ID,
    DEVICE_CLASS_APPARENT_POWER,
    DEVICE_CLASS_CURRENT,
    DEVICE_CLASS_DURATION,
    DEVICE_CLASS_ENERGY,
    DEVICE_CLASS_FREQUENCY,
    DEVICE_CLASS_POWER,
    DEVICE_CLASS_POWER_FACTOR,
    DEVICE_CLASS_REACTIVE_ENERGY,
    DEVICE_CLASS_REACTIVE_POWER,
    DEVICE_CLASS_SIGNAL_STRENGTH,
    DEVICE_CLASS_VOLTAGE,
    ENTITY_CATEGORY_DIAGNOSTIC,
    STATE_CLASS_MEASUREMENT,
    STATE_CLASS_TOTAL,
    STATE_CLASS_TOTAL_INCREASING,
    UNIT_AMPERE,
    UNIT_HERTZ,
    UNIT_KILOWATT,
    UNIT_KILOWATT_HOURS,
    UNIT_SECOND,
    UNIT_VOLT,
)

DEPENDENCIES = ["ble_client", "esp32_ble_tracker"]
AUTO_LOAD = ["sensor", "text_sensor"]
CODEOWNERS = []

m2m_ns = cg.esphome_ns.namespace("m2m_display")
M2MDisplay = m2m_ns.class_(
    "M2MDisplay", cg.PollingComponent, ble_client.BLEClientNode, esp32_ble_tracker.ESPBTDeviceListener
)

CONF_READ_INTERVAL = "read_interval"
CONF_KEEPALIVE_INTERVAL = "keepalive_interval"
CONF_LOG_FRAMES = "log_frames"
CONF_LOG_MIN_INTERVAL = "log_min_interval"
CONF_SCREEN_TIMEOUT = "screen_timeout"
CONF_REPORT_UNKNOWN = "report_unknown"
CONF_EVERY = "every"
CONF_FOUND_METERS = "found_meters"
CONF_DISCOVERY_NAME = "discovery_name"

UNIT_KVARH = "kvarh"
UNIT_KVAR = "kvar"
UNIT_KVA = "kVA"
UNIT_MA = "mA"

# поля разбора экрана (Field в m2m_display.h)
F_VALUE, F_TEXTNUM, F_PHASE, F_KEYNUM, F_KEYTEXT, F_TEXT, F_TARIFF = range(7)
# как сравнивать заголовок (Match в m2m_display.h)
M_EXACT, M_PREFIX, M_ID = range(3)

_EVERY = {cv.Optional(CONF_EVERY, default=1): cv.int_range(min=1, max=1000)}


def _num(unit=None, dc=None, sc=STATE_CLASS_MEASUREMENT, dec=2, icon=None, cat=None):
    kw = {"accuracy_decimals": dec, "state_class": sc}
    if unit:
        kw["unit_of_measurement"] = unit
    if dc:
        kw["device_class"] = dc
    if icon:
        kw["icon"] = icon
    if cat:
        kw["entity_category"] = cat
    return lambda: sensor.sensor_schema(**kw).extend(_EVERY)


def _txt(icon=None, cat=None):
    kw = {}
    if icon:
        kw["icon"] = icon
    if cat:
        kw["entity_category"] = cat
    return lambda: text_sensor.text_sensor_schema(**kw).extend(_EVERY)


_E_TOT = _num(UNIT_KILOWATT_HOURS, DEVICE_CLASS_ENERGY, STATE_CLASS_TOTAL_INCREASING)
_E_MON = _num(UNIT_KILOWATT_HOURS, DEVICE_CLASS_ENERGY, STATE_CLASS_TOTAL)
_R_TOT = _num(UNIT_KVARH, DEVICE_CLASS_REACTIVE_ENERGY, STATE_CLASS_TOTAL_INCREASING)
_R_MON = _num(UNIT_KVARH, DEVICE_CLASS_REACTIVE_ENERGY, STATE_CLASS_TOTAL)
_KW = _num(UNIT_KILOWATT, DEVICE_CLASS_POWER)
_KVAR = _num(UNIT_KVAR, DEVICE_CLASS_REACTIVE_POWER)
_KVA = _num(UNIT_KVA, DEVICE_CLASS_APPARENT_POWER)
_A = _num(UNIT_AMPERE, DEVICE_CLASS_CURRENT, dec=3)
_V = _num(UNIT_VOLT, DEVICE_CLASS_VOLTAGE, dec=1)
_PF = _num(None, DEVICE_CLASS_POWER_FACTOR, dec=3)

# (ключ yaml, схема, поле, заголовок, сравнение, экран-хозяин подменю, аргумент, «живой» экран, проверено)
#   хозяин: "" — экран верхнего уровня; "main" — подменю начального экрана (1.2 … 1.7);
#           "#1.6" — подменю экрана 1.6; иначе — заголовок экрана верхнего уровня, из которого
#           долгим «вправо» открывается подменю.
#   «живой» — значение меняется каждый кадр (ток, мощность), читается со второго кадра.
CATALOG = []


def _add(key, schema, field, title, match=M_EXACT, host="", arg="", live=False, tested=False):
    CATALOG.append((key, schema, field, title, match, host, arg, live, tested))


_ENERGY = [
    # ключ итога, ключ для тарифов, заголовок, схема итога, схема тарифов
    ("energy_import_total", "energy_import", "А.ЭНЕРГИЯ+ОБЩ.", _E_TOT, _E_TOT),
    ("energy_export_total", "energy_export", "А.ЭНЕРГИЯ-ОБЩ.", _E_TOT, _E_TOT),
    ("reactive_import_total", "reactive_import", "Р.ЭНЕРГИЯ+ОБЩ.", _R_TOT, _R_TOT),
    ("reactive_export_total", "reactive_export", "Р.ЭНЕРГИЯ-ОБЩ.", _R_TOT, _R_TOT),
    ("energy_import_month", "energy_import_month", "А.ЭНЕРГИЯ+МЕС.", _E_MON, _E_MON),
    ("energy_export_month", "energy_export_month", "А.ЭНЕРГИЯ-МЕС.", _E_MON, _E_MON),
    ("reactive_import_month", "reactive_import_month", "Р.ЭНЕРГИЯ+МЕС.", _R_MON, _R_MON),
    ("reactive_export_month", "reactive_export_month", "Р.ЭНЕРГИЯ-МЕС.", _R_MON, _R_MON),
]
for total_key, tkey, title, s_total, s_t in _ENERGY:
    _add(total_key, s_total, F_VALUE, title, tested=True)
    for n in range(1, 5):
        # подменю экранов 2–9: тарифы Т1–Т4 («А.ЭНЕРГИЯ+ОБЩ.Т1»)
        _add(f"{tkey}_t{n}", s_t, F_VALUE, f"{title}Т{n}", host=title,
             tested=n <= 2 and title in ("А.ЭНЕРГИЯ+ОБЩ.", "А.ЭНЕРГИЯ-МЕС."))

# экран 10 и его подменю
_add("max_power_month", _KW, F_VALUE, "МАКС.МОЩНОСТИМЕС.", tested=True)
for ph in "abc":
    _add(f"max_power_month_{ph}", _num(UNIT_KVA, DEVICE_CLASS_APPARENT_POWER), F_PHASE, "МАКС.МОЩНОСТИМЕС.",
         arg=ph.upper())
_add("max_power_today", _KW, F_VALUE, "МАКС.МОЩ.", M_PREFIX, host="МАКС.МОЩНОСТИМЕС.", tested=True)

# экран 11 и его подменю
_add("power_factor", _PF, F_VALUE, "ПАРАМЕТРЫНАГР.", live=True, tested=True)
for ph in "abc":
    _add(f"power_factor_{ph}", _PF, F_PHASE, "ПАРАМЕТРЫНАГР.", arg=ph.upper(), live=True)
_SUB11 = [
    ("current", _A, "ТЕКУЩЕЕЗНАЧЕНИЕТОКА"),
    ("active_power", _KW, "ТЕК.ЗНАЧ.А.МОЩ."),
    ("reactive_power", _KVAR, "ТЕК.ЗНАЧ.Р.МОЩ."),
    ("apparent_power", _KVA, "ТЕК.ЗНАЧ.П.МОЩ."),
]
for key, schema, title in _SUB11:
    _add(key, schema, F_VALUE, title, host="ПАРАМЕТРЫНАГР.", live=True, tested=True)
    for ph in "abc":
        _add(f"{key}_{ph}", schema, F_PHASE, title, host="ПАРАМЕТРЫНАГР.", arg=ph.upper(), live=True)
_add("differential_current", _num(UNIT_MA, DEVICE_CLASS_CURRENT, dec=0), F_KEYNUM, "ТЕКУЩЕЕЗНАЧЕНИЕТОКА",
     host="ПАРАМЕТРЫНАГР.", arg="ДИФФ.ТОК", live=True, tested=True)

# экран 12
_add("frequency", _num(UNIT_HERTZ, DEVICE_CLASS_FREQUENCY), F_VALUE, "ПАРАМЕТРЫСЕТИ", live=True, tested=True)
_add("voltage", _V, F_TEXTNUM, "ПАРАМЕТРЫСЕТИ", live=True, tested=True)
for ph in "abc":
    _add(f"voltage_{ph}", _V, F_PHASE, "ПАРАМЕТРЫСЕТИ", arg=ph.upper(), live=True)

# экраны 13–16: заголовков не видели — узнаём по номеру экрана
_add("events_count", _num(None, None, dec=0, icon="mdi:format-list-numbered"), F_VALUE, "13", M_ID)
_add("events_text", _txt("mdi:format-list-bulleted", ENTITY_CATEGORY_DIAGNOSTIC), F_TEXT, "13", M_ID)
_add("tariff_schedule", _txt("mdi:calendar-clock"), F_TEXT, "14", M_ID)
_add("relay_state", _txt("mdi:electric-switch"), F_TEXT, "15", M_ID)  # только чтение: подменю 15 — реле!
_add("message", _txt("mdi:message-text-outline"), F_TEXT, "16", M_ID)

# начальный экран и его подменю 1.2 … 1.6
_add("current_tariff", _txt("mdi:clock-time-four-outline"), F_TARIFF, "ТЕКУЩИЙ", M_PREFIX, live=True,
     tested=True)
_add("serial_number", _txt("mdi:identifier", ENTITY_CATEGORY_DIAGNOSTIC), F_KEYTEXT, "1.2", M_ID, host="main",
     arg="S/N:")
_add("manufacture_date", _txt("mdi:calendar", ENTITY_CATEGORY_DIAGNOSTIC), F_KEYTEXT, "1.2", M_ID, host="main",
     arg="ПРОИЗВЕДЕНО")
_add("passport_text", _txt("mdi:card-account-details-outline", ENTITY_CATEGORY_DIAGNOSTIC), F_TEXT, "1.2",
     M_ID, host="main")
_add("firmware_text", _txt("mdi:chip", ENTITY_CATEGORY_DIAGNOSTIC), F_TEXT, "1.3", M_ID, host="main")
_add("diagnostics_text", _txt("mdi:stethoscope", ENTITY_CATEGORY_DIAGNOSTIC), F_TEXT, "1.4", M_ID, host="main")
_add("uptime", _num(UNIT_SECOND, DEVICE_CLASS_DURATION, STATE_CLASS_TOTAL_INCREASING, dec=0,
                    cat=ENTITY_CATEGORY_DIAGNOSTIC), F_TEXTNUM, "1.5", M_ID, host="main", live=True, tested=True)
_add("gsm_rssi", _num(None, DEVICE_CLASS_SIGNAL_STRENGTH, dec=0, cat=ENTITY_CATEGORY_DIAGNOSTIC), F_KEYNUM,
     "RЕV:", M_PREFIX, host="#1.6", arg="RSSI:")
_add("gsm_ber", _num(None, None, dec=0, icon="mdi:signal", cat=ENTITY_CATEGORY_DIAGNOSTIC), F_KEYNUM,
     "RЕV:", M_PREFIX, host="#1.6", arg="BER:")

_KEYS = {c[0]: c for c in CATALOG}
assert len(_KEYS) == len(CATALOG), "повтор ключа в каталоге"


CONFIG_SCHEMA = (
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(M2MDisplay),
            cv.Optional(CONF_READ_INTERVAL, default="150ms"): cv.positive_time_period_milliseconds,
            cv.Optional(CONF_KEEPALIVE_INTERVAL, default="8s"): cv.positive_time_period_milliseconds,
            cv.Optional(CONF_SCREEN_TIMEOUT, default="9s"): cv.positive_time_period_milliseconds,
            cv.Optional(CONF_LOG_FRAMES, default=False): cv.boolean,
            cv.Optional(CONF_LOG_MIN_INTERVAL, default="5s"): cv.positive_time_period_milliseconds,
            cv.Optional(CONF_REPORT_UNKNOWN, default=True): cv.boolean,
            cv.Optional(CONF_DISCOVERY_NAME, default="M2M"): cv.string,
            cv.Optional(CONF_FOUND_METERS): text_sensor.text_sensor_schema(
                icon="mdi:bluetooth-audio", entity_category=ENTITY_CATEGORY_DIAGNOSTIC
            ),
            **{cv.Optional(c[0]): c[1]() for c in CATALOG},
        }
    )
    .extend(ble_client.BLE_CLIENT_SCHEMA)
    .extend(esp32_ble_tracker.ESP_BLE_DEVICE_SCHEMA)
    .extend(cv.polling_component_schema("1h"))
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await ble_client.register_ble_node(var, config)
    await esp32_ble_tracker.register_ble_device(var, config)
    cg.add(var.set_read_interval(config[CONF_READ_INTERVAL]))
    cg.add(var.set_keepalive_interval(config[CONF_KEEPALIVE_INTERVAL]))
    cg.add(var.set_log_frames(config[CONF_LOG_FRAMES]))
    cg.add(var.set_log_min_interval(config[CONF_LOG_MIN_INTERVAL]))
    cg.add(var.set_screen_timeout(config[CONF_SCREEN_TIMEOUT]))
    cg.add(var.set_report_unknown(config[CONF_REPORT_UNKNOWN]))
    cg.add(var.set_discovery_name(config[CONF_DISCOVERY_NAME]))
    if CONF_FOUND_METERS in config:
        ts = await text_sensor.new_text_sensor(config[CONF_FOUND_METERS])
        cg.add(var.set_found_sensor(ts))
    for key, _, field, title, match, host, arg, live, _ in CATALOG:
        if key not in config:
            continue
        conf = config[key]
        if field in (F_TEXT, F_TARIFF, F_KEYTEXT):
            ts = await text_sensor.new_text_sensor(conf)
            cg.add(var.add_text_entry(key, title, match, host, field, arg, live, conf[CONF_EVERY], ts))
        else:
            sens = await sensor.new_sensor(conf)
            cg.add(var.add_entry(key, title, match, host, field, arg, live, conf[CONF_EVERY], sens))
