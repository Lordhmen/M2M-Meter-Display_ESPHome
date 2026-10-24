/*
 * m2m-display-card — экран счётчика М2М-1С / М2М-3Т и его кнопки в Home Assistant.
 * Работает с компонентом ESPHome m2m_display (блок live_view):
 *   switch  …_m2m_live            — «Живой экран»
 *   sensor  …_m2m_frame_1 … _4     — кадр экрана, 4 страницы по 160 байт в base64
 *   sensor  …_m2m_screen           — распознанный текст экрана
 *   button  …_m2m_left / _right / _long_left / _long_right
 *
 *   type: custom:m2m-display-card
 *   entity: switch.schetchik_ee_zhivoi_ekran_m2m_live   # остальное карточка найдёт сама
 *   title: Счётчик                                      # необязательно
 */
const VERSION = "1.0.0";
const W = 160, H = 32;
const LONG_MS = 700;           // удержание дольше — долгое нажатие
const KEEPALIVE_MS = 60000;    // пока карточка видна, раз в минуту продлеваем «Живой экран»

const SUFFIX = {
  frames: ["m2m_frame_1", "m2m_frame_2", "m2m_frame_3", "m2m_frame_4"],
  screen: "m2m_screen",
  left: "m2m_left", right: "m2m_right", long_left: "m2m_long_left", long_right: "m2m_long_right",
};

class M2MDisplayCard extends HTMLElement {
  static getStubConfig(hass) {
    const sw = Object.keys(hass.states).find((e) => e.startsWith("switch.") && e.endsWith("m2m_live"));
    return { entity: sw || "switch.m2m_live" };
  }

  setConfig(config) {
    if (!config || !config.entity) throw new Error("Укажите entity: выключатель «Живой экран» (…_m2m_live)");
    this._config = { auto_live: true, ...config };
    this._ids = null;
    if (this._root) this._render();
  }

  set hass(hass) {
    const first = !this._hass;
    this._hass = hass;
    if (!this._ids) this._ids = this._resolve();
    if (!this._root) this._build();
    this._update();
    if (first) this._startLive();
  }

  getCardSize() { return 4; }
  getGridOptions() { return { columns: 12, min_columns: 6, rows: 4, min_rows: 3 }; }

  connectedCallback() {
    this._onVis = () => { if (!document.hidden) this._startLive(); };
    document.addEventListener("visibilitychange", this._onVis);
    if (this._hass) this._startLive();
  }

  disconnectedCallback() {
    document.removeEventListener("visibilitychange", this._onVis);
    clearInterval(this._keep);
    this._keep = null;
  }

  // ---- поиск сущностей того же устройства ----------------------------------------------
  _resolve() {
    const c = this._config, h = this._hass;
    const sw = c.entity;
    const ids = { live: sw };
    let pool = [];
    const reg = h.entities && h.entities[sw];
    if (reg && reg.device_id) {
      pool = Object.values(h.entities).filter((e) => e.device_id === reg.device_id).map((e) => e.entity_id);
    } else {
      const prefix = sw.split(".")[1].replace(/m2m_live$/, "");
      pool = Object.keys(h.states).filter((e) => e.split(".")[1].startsWith(prefix));
    }
    const find = (domain, suf) =>
      c[suf] || pool.find((e) => e.startsWith(domain + ".") && e.endsWith(suf)) || null;
    ids.frames = (c.frames || SUFFIX.frames.map((s) => find("sensor", s)));
    ids.screen = c.screen || find("sensor", SUFFIX.screen);
    for (const k of ["left", "right", "long_left", "long_right"]) ids[k] = c[k] || find("button", SUFFIX[k]);
    return ids;
  }

  // ---- разметка -------------------------------------------------------------------------
  _build() {
    this._root = this.attachShadow({ mode: "open" });
    this._render();
  }

  _render() {
    const title = this._config.title ?? "Экран счётчика";
    this._root.innerHTML = `
      <style>
        :host { --lcd-bg: #a7b98e; --lcd-off: rgba(30, 45, 25, 0.07); --lcd-on: #18241a; }
        ha-card { padding: 12px 16px 16px; }
        .head { display: flex; align-items: center; justify-content: space-between; gap: 8px; margin-bottom: 10px; }
        .title { font-size: 16px; font-weight: 500; color: var(--primary-text-color); }
        .state { font-size: 12px; color: var(--secondary-text-color); display: flex; align-items: center; gap: 6px; }
        .dot { width: 8px; height: 8px; border-radius: 50%; background: var(--disabled-text-color, #999); }
        .dot.on { background: var(--success-color, #43a047); }
        .lcdwrap { background: var(--lcd-bg); border-radius: 10px; padding: 10px;
                   box-shadow: inset 0 2px 6px rgba(0,0,0,.25); position: relative; }
        canvas { display: block; width: 100%; height: auto; image-rendering: pixelated; }
        .overlay { position: absolute; inset: 0; display: flex; align-items: center; justify-content: center;
                   border-radius: 10px; background: rgba(167,185,142,.85); color: #18241a; font-size: 14px;
                   text-align: center; padding: 8px; }
        .overlay[hidden] { display: none; }
        .overlay button { margin-left: 8px; }
        .text { margin: 10px 2px 0; min-height: 18px; font-size: 13px; color: var(--secondary-text-color);
                overflow-wrap: anywhere; }
        .btns { display: grid; grid-template-columns: 1fr 1fr; gap: 12px; margin-top: 12px; }
        button { font: inherit; cursor: pointer; border: none; border-radius: 10px;
                 background: var(--secondary-background-color, #eceff1); color: var(--primary-text-color, #212121);
                 padding: 12px 8px; user-select: none; -webkit-user-select: none; touch-action: manipulation;
                 display: flex; align-items: center; justify-content: center; gap: 6px; position: relative;
                 overflow: hidden; }
        button:active { filter: brightness(0.92); }
        button .fill { position: absolute; left: 0; bottom: 0; height: 3px; width: 0; background: var(--primary-color); }
        button.holding .fill { width: 100%; transition: width ${LONG_MS}ms linear; }
        button.long { outline: 2px solid var(--primary-color); }
        .hint { margin-top: 8px; font-size: 11px; color: var(--secondary-text-color); text-align: center; }
        .small { padding: 4px 10px; border-radius: 8px; font-size: 12px; background: var(--primary-color);
                 color: var(--text-primary-color, #fff); }
      </style>
      <ha-card>
        <div class="head">
          <div class="title">${title}</div>
          <div class="state"><span class="dot"></span><span class="st">—</span></div>
        </div>
        <div class="lcdwrap">
          <canvas width="${W * 4}" height="${H * 4}"></canvas>
          <div class="overlay" hidden><span class="ovtext"></span></div>
        </div>
        <div class="text"></div>
        <div class="btns">
          <button class="b" data-dir="left"><ha-icon icon="mdi:chevron-left"></ha-icon><span>Влево</span><i class="fill"></i></button>
          <button class="b" data-dir="right"><span>Вправо</span><ha-icon icon="mdi:chevron-right"></ha-icon><i class="fill"></i></button>
        </div>
        <div class="hint">Коротко — листать, удерживать — войти в подменю / выйти из него.
          Долгое «вправо» на экранах 15 (реле) и 1.7 (модем) не сработает — так задумано.</div>
      </ha-card>`;
    this._canvas = this._root.querySelector("canvas");
    this._ctx = this._canvas.getContext("2d");
    this._lastFrame = null;
    for (const b of this._root.querySelectorAll("button.b")) this._bindButton(b);
    if (this._hass) this._update();
  }

  _bindButton(b) {
    let timer = null, long = false, down = false;
    const start = (ev) => {
      ev.preventDefault();
      down = true; long = false;
      b.classList.add("holding");
      timer = setTimeout(() => { long = true; b.classList.add("long"); }, LONG_MS);
    };
    const end = (ev) => {
      if (!down) return;
      down = false;
      clearTimeout(timer);
      b.classList.remove("holding", "long");
      if (ev.type === "pointerleave" || ev.type === "pointercancel") return;
      const dir = b.dataset.dir;
      this._press(long ? "long_" + dir : dir);
    };
    b.addEventListener("pointerdown", start);
    b.addEventListener("pointerup", end);
    b.addEventListener("pointerleave", end);
    b.addEventListener("pointercancel", end);
    b.addEventListener("contextmenu", (e) => e.preventDefault());
  }

  // ---- действия -------------------------------------------------------------------------
  _press(kind) {
    const id = this._ids && this._ids[kind];
    if (!id) return this._toast(`Не найдена кнопка …_m2m_${kind}`);
    this._hass.callService("button", "press", { entity_id: id });
    this._startLive();
  }

  _startLive() {
    if (!this._hass || !this._config.auto_live || document.hidden) return;
    const sw = this._ids && this._ids.live;
    if (!sw || !this._hass.states[sw]) return;
    const now = Date.now();
    if (!this._lastOn || now - this._lastOn > 5000) {  // продлеваем, но не чаще раза в 5 с
      this._lastOn = now;
      this._hass.callService("switch", "turn_on", { entity_id: sw });
    }
    if (!this._keep) this._keep = setInterval(() => { this._lastOn = 0; this._startLive(); }, KEEPALIVE_MS);
  }

  _toast(msg) {
    this.dispatchEvent(new CustomEvent("hass-notification", { detail: { message: msg }, bubbles: true, composed: true }));
  }

  // ---- отрисовка ------------------------------------------------------------------------
  _update() {
    if (!this._root || !this._hass || !this._ids) return;
    const h = this._hass, ids = this._ids;
    const sw = h.states[ids.live];
    const on = sw && sw.state === "on";
    const avail = sw && sw.state !== "unavailable";
    const dot = this._root.querySelector(".dot"), st = this._root.querySelector(".st");
    const ov = this._root.querySelector(".overlay"), ovt = this._root.querySelector(".ovtext");

    // кадр
    const parts = ids.frames.map((e) => (e && h.states[e] ? h.states[e].state : ""));
    const key = parts.join("|");
    let ok = parts.every((p) => p && p.length >= 200 && p !== "unknown" && p !== "unavailable");
    if (ok && key !== this._lastFrame) {
      this._lastFrame = key;
      ok = this._draw(parts);
    }
    const ages = ids.frames.map((e) => (e && h.states[e] ? Date.now() - new Date(h.states[e].last_updated) : 1e12));
    const age = Math.min(...ages) / 1000;

    if (!avail) {
      dot.className = "dot"; st.textContent = "нет связи с ESP";
    } else if (!on) {
      dot.className = "dot"; st.textContent = "живой экран выключен";
    } else {
      dot.className = "dot on";
      st.textContent = age < 15 ? "на связи" : `кадр ${Math.round(age)} с назад`;
    }
    if (!avail) { ov.hidden = false; ovt.textContent = "ESP недоступна"; }
    else if (!this._lastFrame) { ov.hidden = false; ovt.textContent = on ? "Жду кадр с экрана…" : "Включаю живой экран…"; }
    else if (!on) { ov.hidden = false; ovt.innerHTML = `Показан последний кадр <button class="small">Обновить</button>`;
                    ov.querySelector("button").onclick = () => { this._lastOn = 0; this._startLive(); }; }
    else ov.hidden = true;

    const scr = ids.screen && h.states[ids.screen];
    this._root.querySelector(".text").textContent =
      scr && !["unknown", "unavailable"].includes(scr.state) ? scr.state : "";
  }

  _draw(parts) {
    const bytes = [];
    try {
      for (const p of parts) {
        const bin = atob(p);
        if (bin.length !== 160) return false;
        for (let i = 0; i < bin.length; i++) bytes.push(bin.charCodeAt(i));
      }
    } catch (e) { return false; }
    const ctx = this._ctx, S = 4, cs = getComputedStyle(this);
    const on = cs.getPropertyValue("--lcd-on").trim() || "#18241a";
    const off = cs.getPropertyValue("--lcd-off").trim() || "rgba(30,45,25,.07)";
    ctx.clearRect(0, 0, W * S, H * S);
    for (let y = 0; y < H; y++) {
      const page = y >> 3, bit = y & 7;
      for (let x = 0; x < W; x++) {
        const lit = (bytes[page * W + x] >> bit) & 1;
        ctx.fillStyle = lit ? on : off;
        ctx.fillRect(x * S, y * S, S - 1, S - 1);
      }
    }
    return true;
  }
}

customElements.define("m2m-display-card", M2MDisplayCard);
window.customCards = window.customCards || [];
window.customCards.push({
  type: "m2m-display-card",
  name: "Экран счётчика М2М",
  description: "Копия экрана счётчика М2М-1С/3Т и его кнопки (ESPHome m2m_display, live_view)",
  preview: false,
});
console.info(`%c M2M-DISPLAY-CARD %c ${VERSION} `, "background:#18241a;color:#a7b98e", "background:#a7b98e;color:#18241a");
