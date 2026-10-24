#pragma once
// Распознавание экрана счётчиков М2М (160x32 точек, 4 страницы по 160 байт, бит 0 — верхняя точка страницы).
//  * заголовок — верхняя строка шрифтом 5x7 (номер экрана и название);
//  * крупные цифры (высота 22) и мелкие цифры (высота 13) — значение экрана;
//  * строки текста шрифтом 5x7 в теле экрана (единицы, напряжение, фазы, надписи).
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include "glyphs.h"
#include "font5x7.h"

namespace esphome {
namespace m2m_display {

struct TextLine {
  uint8_t x0, y0;     // левый верхний угол строки
  std::string text;   // слова разделены пробелом; '?' — незнакомый знак
};

struct UnknownGlyph {
  uint8_t w;
  uint8_t cols[8];
};

struct ScreenData {
  std::string id;      // номер экрана в заголовке: "2", "12", "1.5"; пусто — главный экран или подменю
  std::string title;   // заголовок без пробелов: «2.А.ЭНЕРГИЯ+ОБЩ.»; '?' — незнакомый знак
  std::string title_sp;  // заголовок с пробелами между словами — для лога
  std::string big;     // крупные цифры (целая часть значения)
  std::string small;   // мелкие цифры (дробная часть)
  std::string text;    // цифры шрифтом 5x7 в теле экрана подряд (как в первой версии — напряжение на экране 12)
  std::vector<TextLine> lines;  // строки текста в теле экрана, сверху вниз
  bool ok{false};      // все крупные и мелкие цифры распознаны
  uint32_t hdr{0};     // хэш верхней строки
  std::vector<UnknownGlyph> unknown;  // незнакомые знаки 5x7 (для отчёта)
  bool operator==(const ScreenData &o) const {
    if (hdr != o.hdr || id != o.id || big != o.big || small != o.small || text != o.text || ok != o.ok ||
        lines.size() != o.lines.size())
      return false;
    for (size_t i = 0; i < lines.size(); i++)
      if (lines[i].text != o.lines[i].text)
        return false;
    return true;
  }
  bool operator!=(const ScreenData &o) const { return !(*this == o); }
};

class ScreenDecoder {
 public:
  static const int W = 160;
  static const int H = 32;
  static const int MAX_COMPS = 220;
  static const int MAX_LINES = 16;

  ScreenData decode(const uint8_t *f) {
    ScreenData out;
    uint32_t h = 0x811C9DC5u;  // FNV-1a по первым 136 столбцам верхней страницы
    for (int i = 0; i < 136; i++)
      h = (h ^ f[i]) * 0x01000193u;
    out.hdr = h;
    read_title_(f, out);
    this->label_(f);

    // номер экрана: цифры и точки в начале заголовка
    int order[MAX_COMPS];
    int n = 0;
    for (int i = 0; i < this->n_; i++)
      if (this->c_[i].y1 <= 7)
        order[n++] = i;
    this->sort_by_x_(order, n);
    int lastx = -1;
    for (int k = 0; k < n; k++) {
      Comp &c = this->c_[order[k]];
      if (lastx >= 0 && c.x0 - lastx > 4)
        break;
      int w = c.x1 - c.x0 + 1, hh = c.y1 - c.y0 + 1;
      if (c.ch)
        out.id += c.ch;
      else if (c.y0 >= 5 && w <= 2 && hh <= 2)
        out.id += '.';
      else
        break;
      lastx = c.x1;
    }
    while (!out.id.empty() && out.id.back() == '.')
      out.id.pop_back();

    // значение: крупные и мелкие цифры
    bool ok = true;
    n = 0;
    for (int i = 0; i < this->n_; i++)
      if (this->c_[i].y0 >= 8)
        order[n++] = i;
    this->sort_by_x_(order, n);
    for (int k = 0; k < n; k++) {
      Comp &c = this->c_[order[k]];
      int hh = c.y1 - c.y0 + 1;
      if (hh == 22) {
        out.big += c.ch ? c.ch : '?';
        ok &= c.ch != 0;
      } else if (hh == 13) {
        out.small += c.ch ? c.ch : '?';
        ok &= c.ch != 0;
      } else if (hh <= 7 && c.ch) {
        out.text += c.ch;
      }
    }
    out.ok = ok;
    this->read_lines_(order, n, out);
    return out;
  }

  // Знак шрифта 5x7 по столбцам; nullptr — незнакомый
  static const char *match_font(const uint8_t *cols, int w) {
    for (int g = 0; g < FONT5X7_COUNT; g++) {
      const FontGlyph &fg = FONT5X7[g];
      if (fg.w != w)
        continue;
      bool eq = true;
      for (int i = 0; i < w && eq; i++)
        eq = fg.cols[i] == cols[i];
      if (eq)
        return fg.ch;
    }
    return nullptr;
  }

 protected:
  // Разбор строки по столбцам: знаки разделены пустыми столбцами, промежуток больше 2 точек — пробел.
  static void segment_(const uint8_t *cols, int n, std::string &with_spaces, std::string &bare,
                       std::vector<UnknownGlyph> *unknown, int *known_count) {
    int x = 0, last = -1;
    while (x < n) {
      if (cols[x] == 0) {
        x++;
        continue;
      }
      int s = x;
      while (x < n && cols[x] != 0)
        x++;
      int w = x - s;
      const char *ch = w <= 5 ? match_font(cols + s, w) : nullptr;
      if (last >= 0 && s - last > 2)
        with_spaces += ' ';
      if (ch != nullptr) {
        with_spaces += ch;
        bare += ch;
        if (known_count)
          (*known_count)++;
      } else {
        with_spaces += '?';
        bare += '?';
        if (unknown != nullptr && w <= 8 && unknown->size() < 8) {
          UnknownGlyph u{};
          u.w = w;
          for (int i = 0; i < w; i++)
            u.cols[i] = cols[s + i];
          unknown->push_back(u);
        }
      }
      last = x;
    }
  }

  // Заголовок — биты 0–6 страницы 0, столбцы 0–135 (правее — значки антенны, батареи).
  static void read_title_(const uint8_t *f, ScreenData &out) {
    uint8_t cols[136];
    for (int x = 0; x < 136; x++)
      cols[x] = f[x] & 0x7F;
    segment_(cols, 136, out.title_sp, out.title, &out.unknown, nullptr);
  }

  // Строки 5x7 в теле экрана: связные области высотой до 8 точек (не цифры значения),
  // собранные в строки по высоте и близости по горизонтали.
  void read_lines_(const int *order, int n, ScreenData &out) {
    struct L {
      int x0, x1, y0, y1;
    };
    L ls[MAX_LINES];
    int nl = 0;
    for (int i = 0; i < this->n_; i++)
      this->line_of_[i] = -1;
    for (int k = 0; k < n; k++) {
      int i = order[k];
      Comp &c = this->c_[i];
      int hh = c.y1 - c.y0 + 1;
      if (hh > 8)
        continue;
      int best = -1;
      for (int j = 0; j < nl; j++)
        if (c.y0 <= ls[j].y1 && c.y1 >= ls[j].y0 && c.x0 - ls[j].x1 <= 8)
          best = j;
      if (best < 0) {
        if (nl >= MAX_LINES)
          continue;
        best = nl++;
        ls[best] = L{c.x0, c.x1, c.y0, c.y1};
      } else {
        L &l = ls[best];
        if (c.x1 > l.x1) l.x1 = c.x1;
        if (c.y0 < l.y0) l.y0 = c.y0;
        if (c.y1 > l.y1) l.y1 = c.y1;
      }
      this->line_of_[i] = best;
    }
    for (int j = 0; j < nl; j++) {
      // нижняя строка знаков — самая частая нижняя граница областей строки
      int cnt[H] = {0};
      for (int i = 0; i < this->n_; i++)
        if (this->line_of_[i] == j)
          cnt[this->c_[i].y1]++;
      int base = ls[j].y1;
      for (int y = 0; y < H; y++)
        if (cnt[y] > cnt[base])
          base = y;
      int top = base - 6;
      int w = ls[j].x1 - ls[j].x0 + 1;
      uint8_t cols[W];
      for (int x = 0; x < w; x++) {
        uint8_t v = 0;
        for (int r = 0; r < 7; r++) {
          int y = top + r;
          if (y < 0 || y >= H)
            continue;
          uint8_t id = this->lab_[y * W + ls[j].x0 + x];
          if (id && this->line_of_[id - 1] == j)
            v |= 1 << r;
        }
        cols[x] = v;
      }
      TextLine tl;
      tl.x0 = ls[j].x0;
      tl.y0 = top < 0 ? 0 : top;
      std::string bare;
      int known = 0;
      std::vector<UnknownGlyph> unk;
      segment_(cols, w, tl.text, bare, &unk, &known);
      if (known == 0)
        continue;  // одни значки (антенна, батарея, стрелки) — не текст
      for (auto &u : unk)
        if (out.unknown.size() < 8)
          out.unknown.push_back(u);
      out.lines.push_back(tl);
    }
    // сверху вниз, слева направо
    for (size_t a = 1; a < out.lines.size(); a++) {
      TextLine v = out.lines[a];
      size_t b = a;
      while (b > 0 && (out.lines[b - 1].y0 > v.y0 || (out.lines[b - 1].y0 == v.y0 && out.lines[b - 1].x0 > v.x0))) {
        out.lines[b] = out.lines[b - 1];
        b--;
      }
      out.lines[b] = v;
    }
  }

  struct Comp {
    uint8_t x0, x1, y0, y1;
    char ch;
  };

  static inline bool px_(const uint8_t *f, int x, int y) { return (f[(y >> 3) * W + x] >> (y & 7)) & 1; }

  void sort_by_x_(int *a, int n) {
    for (int i = 1; i < n; i++) {
      int v = a[i], j = i - 1;
      while (j >= 0 && this->c_[a[j]].x0 > this->c_[v].x0) {
        a[j + 1] = a[j];
        j--;
      }
      a[j + 1] = v;
    }
  }

  void label_(const uint8_t *f) {
    memset(this->lab_, 0, sizeof(this->lab_));
    this->n_ = 0;
    for (int y = 0; y < H; y++) {
      for (int x = 0; x < W; x++) {
        if (!px_(f, x, y) || this->lab_[y * W + x])
          continue;
        if (this->n_ >= MAX_COMPS)
          return;
        uint8_t id = this->n_ + 1;
        Comp c{(uint8_t) x, (uint8_t) x, (uint8_t) y, (uint8_t) y, 0};
        int sp = 0;
        this->stk_[sp++] = y * W + x;
        this->lab_[y * W + x] = id;
        while (sp) {
          int p = this->stk_[--sp];
          int a = p % W, b = p / W;
          if (a < c.x0) c.x0 = a;
          if (a > c.x1) c.x1 = a;
          if (b < c.y0) c.y0 = b;
          if (b > c.y1) c.y1 = b;
          for (int dy = -1; dy <= 1; dy++)
            for (int dx = -1; dx <= 1; dx++) {
              int u = a + dx, v = b + dy;
              if (u < 0 || u >= W || v < 0 || v >= H)
                continue;
              int q = v * W + u;
              if (this->lab_[q] || !px_(f, u, v))
                continue;
              this->lab_[q] = id;
              this->stk_[sp++] = q;
            }
        }
        c.ch = this->match_(c, id);
        this->c_[this->n_++] = c;
      }
    }
  }

  char match_(const Comp &c, uint8_t id) {
    int w = c.x1 - c.x0 + 1, h = c.y1 - c.y0 + 1;
    if (w > 14 || h > 22)
      return 0;
    uint32_t cols[14];
    for (int i = 0; i < w; i++) {
      uint32_t v = 0;
      for (int j = 0; j < h; j++)
        if (this->lab_[(c.y0 + j) * W + c.x0 + i] == id)
          v |= 1u << j;
      cols[i] = v;
    }
    for (int g = 0; g < GLYPH_COUNT; g++) {
      const Glyph &gl = GLYPHS[g];
      if (gl.w != w || gl.h != h)
        continue;
      bool eq = true;
      for (int i = 0; i < w && eq; i++)
        eq = gl.cols[i] == cols[i];
      if (eq)
        return gl.ch;
    }
    return 0;
  }

  uint8_t lab_[W * H];
  uint16_t stk_[W * H];
  Comp c_[MAX_COMPS];
  int8_t line_of_[MAX_COMPS];
  int n_{0};
};

}  // namespace m2m_display
}  // namespace esphome
