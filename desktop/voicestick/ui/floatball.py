"""悬浮球交互界面 — 主球 + 复制球 + 对角矩形 + 居中文本编辑框"""
import logging
from PyQt5.QtWidgets import QWidget, QLabel, QTextEdit, QApplication, QPushButton, QVBoxLayout, QHBoxLayout, QShortcut
from PyQt5.QtCore import Qt, QTimer, QPoint, QEvent, pyqtSignal, pyqtSlot, QObject
from PyQt5.QtGui import QPainter, QColor, QPen, QRadialGradient, QFont, QKeySequence
import ctypes.wintypes
import ctypes

logger = logging.getLogger(__name__)

BALL_R = 20           # 40px diameter
TEXT_W = 280
TEXT_H = 100
GAP = 4               # vertical gap between elements
CLICK = 5             # drag threshold
SHADOW_R = 16         # padding between ball and window edge

# Colors
C_BALL1 = QColor(160, 220, 245, 200)
C_BALL2 = QColor(135, 206, 235, 140)
C_TEXT = QColor(15, 36, 51)
C_RECT_FILL = QColor(190, 225, 245, 30)
C_RECT_BORDER = QColor(150, 200, 230, 64)
C_TOAST_BG = QColor(40, 40, 40, 165)

# ---- cross-thread bridge ----
class _Bridge(QObject):
    text_signal = pyqtSignal(str, bool, int)
    clear_signal = pyqtSignal()
    status_signal = pyqtSignal(str)


class FloatingBallWindow(QWidget):
    position_changed = pyqtSignal()
    reconnect_requested = pyqtSignal()  # 双击小球触发重连

    def __init__(self):
        super().__init__(None)
        self._text = ""
        self._has_text = False
        self._accumulated = []
        self._text_gen = 0
        self._drag = False
        self._drag_pt = QPoint()

        self.setWindowFlags(
            Qt.WindowStaysOnTopHint | Qt.FramelessWindowHint
            | Qt.Tool)
        self.setAttribute(Qt.WA_ShowWithoutActivating)
        self.setAttribute(Qt.WA_TranslucentBackground)
        self.setMouseTracking(True)

        self._bridge = _Bridge()
        self._bridge.text_signal.connect(self._on_text)
        self._bridge.clear_signal.connect(self.clear_text)
        self._bridge.status_signal.connect(self._on_status)

        # ---- children ----
        self._main = _Ball(self, "")
        self._copy = None  # replaced by side button "复制"

        self._edit = QTextEdit(self)
        self._edit.setVerticalScrollBarPolicy(Qt.ScrollBarAlwaysOff)
        self._edit.setHorizontalScrollBarPolicy(Qt.ScrollBarAlwaysOff)
        self._edit.setFocusPolicy(Qt.NoFocus)  # 禁止获取焦点，防止文字被输入到悬浮球自身
        self._edit.setStyleSheet(
            "QTextEdit{background:rgba(255,255,255,0.5);"
            "color:#1a1a1a;font-size:14px;border:1px solid transparent;"
            "border-radius:8px;padding:8px 10px;}"
            "QTextEdit:focus{border-color:rgba(135,206,235,0.55);"
            "background:rgba(255,255,255,0.35);}")
        self._edit.setAttribute(Qt.WA_ShowWithoutActivating)
        self._edit.hide()
        # 点击文字区域 → 弹出编辑对话框（悬浮球自身禁止激活，无法直接编辑）
        self._edit.installEventFilter(self)

        # ---- buttons (清除/复制/整理/翻译/保存) ----
        self._side_btns = []
        for label in ("清除", "复制", "润色", "翻译", "保存"):
            b = _FuncBtn(label, self)
            b.clicked.connect(lambda checked, lbl=label: self._on_btn(lbl))
            b.hide()
            self._side_btns.append(b)

        self._toast = QLabel("", self)
        self._toast.setStyleSheet(
            "QLabel{color:#fff;background:rgba(40,40,40,0.65);font-size:13px;"
            "border-radius:99px;padding:9px 20px;}")
        self._toast.hide()
        self._toast_timer = QTimer(self)
        self._toast_timer.setSingleShot(True)
        self._toast_timer.timeout.connect(self._toast.hide)

        self._ble_connected = False
        self._pulse = QTimer(self)
        self._pulse.timeout.connect(self._pulse_tick)
        self._pulse_on = False

        # 卡死兜底：非空闲状态超过 15 秒自动回蓝
        self._recover = QTimer(self)
        self._recover.setSingleShot(True)
        self._recover.timeout.connect(self._recover_reset)

        # LLM callbacks (set externally by app.py)
        self._polish_cb = None
        self._translate_cb = None

        self._saved_x = self._saved_y = -1
        self._edit_hint_done = False
        self._pos_small()
        self._init_pos()

    # ========== public API (thread-safe) ==========

    def set_status(self, s):
        self._bridge.status_signal.emit(s)

    def set_partial_text(self, text):
        if text:
            self._bridge.text_signal.emit(text, False, self._text_gen)

    def set_final_text(self, text):
        if text:
            self._bridge.text_signal.emit(text, True, self._text_gen)

    def clear_text(self):
        self._text_gen += 1
        self._accumulated.clear()
        self._text = ""
        self._has_text = False
        self._edit.clear()
        self._edit.hide()
        for b in self._side_btns:
            b.hide()
        self._toast.hide()
        self._pos_small()

    def set_connected(self, connected: bool):
        """Set BLE connection state (shows green dot on main ball)"""
        self._ble_connected = connected
        self._main.set_connected(connected)

    def set_llm_callbacks(self, polish_cb=None, translate_cb=None):
        """Set async callbacks for polish and translate"""
        self._polish_cb = polish_cb
        self._translate_cb = translate_cb

    def load_pos(self, x, y):
        self._saved_x, self._saved_y = x, y

    def save_pos(self):
        return self.x(), self.y()

    # ========== slots (main thread) ==========

    @pyqtSlot(str, bool, int)
    def _on_text(self, text, final, gen):
        if gen < self._text_gen:
            return
        if final:
            self._accumulated.append(text)
            self._text = "\n".join(self._accumulated)
        else:
            self._text = text
            if self._accumulated:
                self._text = "\n".join(self._accumulated) + "\n" + text
        self._show(self._text)

    def _recover_reset(self):
        """15s 卡死兜底：回蓝并清掉录音脉冲留下的红边"""
        self._pulse.stop()
        self._main.set_border(None)
        self._main.set_color(QColor(160, 220, 245, 200))

    @pyqtSlot(str)
    def _on_status(self, s):
        self._main.set_color(self._status_color(s))
        if "录音" in s:
            if not self._pulse.isActive():
                self._pulse.start(500)
        else:
            self._pulse.stop()
            self._main.set_border(None)  # 清除脉冲留下的红边
        # 非空闲状态启动 15s 兜底，空闲状态取消
        busy = ("录音" in s or "识别" in s or "粘贴" in s or
                "翻译" in s or "润色" in s)
        if busy:
            self._recover.start(15000)
        else:
            self._recover.stop()

    @staticmethod
    def _status_color(s):
        if "录音" in s: return QColor(255,100,100,160)
        if "已粘贴" in s: return QColor(50,200,120,160)
        if "识别" in s: return QColor(255,190,50,160)
        if "断开" in s: return QColor(160,160,160,160)
        return QColor(160, 220, 245, 200)

    # ========== layout ==========

    def _pos_small(self):
        self.setFixedSize(88, 88)
        self._main.move(SHADOW_R, SHADOW_R)

    def _show(self, text):
        self._has_text = True
        self._edit.setText(text)
        self._edit.show()
        self._relayout()
        self._clamp()
        self.update()
        # 首次显示文字时提示可点击修改
        if not self._edit_hint_done:
            self._edit_hint_done = True
            self.show_toast("点击文字可修改")

    # ========== 点击文字 → 非模态编辑窗口 ==========

    def eventFilter(self, obj, event):
        if obj is self._edit and event.type() == QEvent.MouseButtonPress:
            if event.button() == Qt.LeftButton:
                self._open_editor()
                return True  # 拦截点击，避免误触发窗口拖动
        return super().eventFilter(obj, event)

    def _open_editor(self):
        text = self._edit.toPlainText()
        if not text.strip():
            self.show_toast("暂无内容")
            return
        # 计算编辑器位置：在悬浮球旁边
        screen = self.screen()
        scr = screen.availableGeometry() if screen else None
        if scr is None:
            self.show_toast("无法打开编辑器")
            return
        cx = self.x() + self.width() // 2
        cy = self.y() + self.height() // 2
        right = cx > scr.width() // 2
        bottom = cy > scr.height() // 2
        # 编辑器窗口位置：悬浮球右侧/左侧展开
        if right:
            ex = self.x() - 460
        else:
            ex = self.x() + self.width() + 4
        ey = self.y() - 20
        # 不超出屏幕范围
        ex = max(scr.left() + 10, min(ex, scr.width() - 470))
        ey = max(scr.top() + 10, min(ey, scr.height() - 240))

        editor = MiniEditor(text, self)
        editor.set_geometry(ex, ey, 460, 220)
        editor.editor_accepted.connect(self._on_editor_accepted)
        editor.show()

    def _on_editor_accepted(self, text: str):
        if text != self._edit.toPlainText():
            self._show(text)
        QApplication.clipboard().setText(text)
        self.show_toast("已修改并复制")

    def _relayout(self):
        """根据屏幕位置重新布局（四象限自适应）"""
        if not self._has_text:
            return
        if self.screen() is None:
            return
        scr = self.screen().availableGeometry()
        cx = self.x() + self.width() // 2
        cy = self.y() + self.height() // 2
        right = cx > scr.width() // 2
        bottom = cy > scr.height() // 2

        bw, bh = 48, 28
        m = SHADOW_R
        bsz = 56  # ball widget size

        if right:
            # 球在右
            bx = m  # text left edge
            if bottom:
                # 右下角：球在右下，文字在球左上方
                w = m + TEXT_W + 8 + bsz + m
                h = m + bh + 4 + TEXT_H + 4 + bsz + m
                self.setFixedSize(w, h)
                self._main.move(w - m - bsz, h - m - bsz)  # bottom-right
                tx, ty = m, m + bh + 4
                self._edit.setGeometry(tx, ty, TEXT_W, TEXT_H)
                # 按钮在文字上方
                total_w = len(self._side_btns) * bw + (len(self._side_btns) - 1) * 4
                btn_x = tx + (TEXT_W - total_w) // 2
                for i, b in enumerate(self._side_btns):
                    b.move(btn_x + i * (bw + 4), m)
                    b.show()
            else:
                # 右上角：球在右上，文字在球左下方
                w = m + TEXT_W + 8 + bsz + m
                h = m + bsz + GAP + TEXT_H + 4 + bh + m
                self.setFixedSize(w, h)
                self._main.move(w - m - bsz, m)  # top-right
                tx, ty = m, m + bsz + GAP
                self._edit.setGeometry(tx, ty, TEXT_W, TEXT_H)
                total_w = len(self._side_btns) * bw + (len(self._side_btns) - 1) * 4
                btn_x = tx + (TEXT_W - total_w) // 2
                for i, b in enumerate(self._side_btns):
                    b.move(btn_x + i * (bw + 4), ty + TEXT_H + 4)
                    b.show()
        else:
            # 球在左
            if bottom:
                # 左下角：球在左下，文字在球右上方
                w = m + bsz + 8 + TEXT_W + m
                h = m + bh + 4 + TEXT_H + 4 + bsz + m
                self.setFixedSize(w, h)
                self._main.move(m, h - m - bsz)  # bottom-left
                tx, ty = m + bsz + 8, m + bh + 4
                self._edit.setGeometry(tx, ty, TEXT_W, TEXT_H)
                total_w = len(self._side_btns) * bw + (len(self._side_btns) - 1) * 4
                btn_x = tx + (TEXT_W - total_w) // 2
                for i, b in enumerate(self._side_btns):
                    b.move(btn_x + i * (bw + 4), m)
                    b.show()
            else:
                # 左上角：球在左上，文字在球右下方（默认）
                w = m + bsz + 8 + TEXT_W + m
                h = m + bsz + GAP + TEXT_H + 4 + bh + m
                self.setFixedSize(w, h)
                self._main.move(m, m)
                tx, ty = m + bsz + 8, m + bsz + GAP
                self._edit.setGeometry(tx, ty, TEXT_W, TEXT_H)
                total_w = len(self._side_btns) * bw + (len(self._side_btns) - 1) * 4
                btn_x = tx + (TEXT_W - total_w) // 2
                for i, b in enumerate(self._side_btns):
                    b.move(btn_x + i * (bw + 4), ty + TEXT_H + 4)
                    b.show()

    def _init_pos(self):
        if self._saved_x > 0:
            self.move(self._saved_x, self._saved_y)
        else:
            self.move(50, 50)
        self._clamp()

    def _clamp(self):
        if self.screen() is None:
            return
        scr = self.screen().availableGeometry()
        x = max(scr.left(), min(self.x(), scr.width() - self.width()))
        y = max(scr.top(), min(self.y(), scr.height() - self.height()))
        self.move(x, y)
        self.position_changed.emit()

    # ========== show event: 强制置顶 ==========

    def showEvent(self, event):
        super().showEvent(event)
        # 通过 Windows API 强制 HWND_TOPMOST，确保不被其他窗口遮挡
        try:
            hwnd = int(self.winId())
            ctypes.windll.user32.SetWindowPos(
                hwnd, -1, 0, 0, 0, 0, 0x0002 | 0x0001)  # HWND_TOPMOST, SWP_NOMOVE | SWP_NOSIZE
        except Exception:
            pass

    # ========== events (drag on main ball only) ==========

    def mousePressEvent(self, e):
        if e.button() == Qt.LeftButton:
            self._drag = True
            self._drag_pt = e.globalPos()

    def mouseMoveEvent(self, e):
        if not self._drag:
            return
        delta = e.globalPos() - self._drag_pt
        if delta.manhattanLength() > CLICK:
            self.move(self.pos() + delta)
            self._drag_pt = e.globalPos()
            # 拖动时实时更新布局（四象限自适应翻转）
            if self._has_text:
                self._relayout()

    def mouseReleaseEvent(self, e):
        if e.button() == Qt.LeftButton and self._drag:
            self._drag = False
            was_drag = (e.globalPos() - self._drag_pt).manhattanLength() > CLICK
            if was_drag and self._has_text:
                self._clamp()
                self._relayout()

    # ========== handlers ==========

    def _on_btn(self, label):
        if label == "清除":
            self.clear_text()
            return
        t = self._edit.toPlainText().strip()
        if label == "复制":
            if not t:
                return
            QApplication.clipboard().setText(t)
            self.show_toast("已复制")
            return
        # 异步操作通过 QTimer.singleShot 在后台运行
        if label == "润色":
            if self._polish_cb and t:
                self._edit.setPlainText("整理中…")
                QTimer.singleShot(50, lambda: self._run_async(self._polish_cb, t))
            elif not t:
                self.show_toast("暂无内容")
            return
        if label == "翻译":
            if self._translate_cb and t:
                self._edit.setPlainText("翻译中…")
                QTimer.singleShot(50, lambda: self._run_async(self._translate_cb, t))
            elif not t:
                self.show_toast("暂无内容")
            return
        if label == "保存":
            if not t:
                self.show_toast("暂无内容")
                return
            try:
                import os, datetime
                path = os.path.join(os.getcwd(), "notes.md")
                with open(path, "a", encoding="utf-8") as f:
                    f.write(f"\n## {datetime.datetime.now().strftime('%Y-%m-%d %H:%M')}\n\n{t}\n")
                self.show_toast(f"已保存到 notes.md")
            except Exception as e:
                self.show_toast(f"保存失败: {e}")

    def _run_async(self, cb, text):
        """Run callback (now sync) and update text on completion"""
        try:
            result = cb(text)
            if result and result.text and not result.error:
                self._edit.setPlainText(result.text)
                self.show_toast(result.text[:20] + "…完成")
            elif result and result.error:
                self._edit.setPlainText(text)
                self.show_toast(result.error)
        except Exception as e:
            self._edit.setPlainText(text)
            self.show_toast(f"失败: {e}")

    def show_toast(self, msg):
        self._toast.setText(msg)
        self._toast.adjustSize()
        self._toast.move((self.width() - self._toast.width()) // 2, 8)
        self._toast.show()
        self._toast_timer.start(1500)

    def _pulse_tick(self):
        self._pulse_on = not self._pulse_on
        c = QColor(255, 100, 100, 200 if self._pulse_on else 100)
        self._main.set_border(c)

    # ========== paint (background rect removed) ==========

    def paintEvent(self, event):
        super().paintEvent(event)
        # background rect layer removed per user request

    # ========== Windows native event: 阻止窗口获得焦点 ==========

    def nativeEvent(self, eventType, message):
        """阻止悬浮球窗口成为前台窗口，确保键盘输入始终发往目标窗口"""
        if eventType == "windows_generic_MSG":
            msg = ctypes.wintypes.MSG.from_address(message.__int__())
            if msg.message == 0x0021:  # WM_MOUSEACTIVATE
                # MA_NOACTIVATE = 3 → 鼠标点击时不激活窗口
                return True, 3
        return super().nativeEvent(eventType, message)


# ========== Ball widget ==========

class _Ball(QWidget):
    def __init__(self, parent, text=""):
        super().__init__(parent)
        self._text = text
        self._color = QColor(160, 220, 245, 200)
        self._border = None
        self._hovered = False
        self._pressed = False
        self._connected = False
        self.setFixedSize(56, 56)  # BALL_R*2 + shadow margin
        self.setCursor(Qt.PointingHandCursor)
        self.setMouseTracking(True)
        self.setAttribute(Qt.WA_TransparentForMouseEvents, False)

    def mouseDoubleClickEvent(self, event):
        """双击小球 → 通知父窗口触发重连"""
        if not self._connected:
            parent = self.parent()
            if hasattr(parent, 'reconnect_requested'):
                parent.reconnect_requested.emit()
        super().mouseDoubleClickEvent(event)

    def set_text(self, t):
        self._text = t
        self.update()

    def set_color(self, c):
        self._color = c
        self.update()

    def set_connected(self, on):
        self._connected = on
        self.update()

    def set_border(self, c):
        self._border = c
        self.update()

    def enterEvent(self, e):
        self._hovered = True
        self.update()
        super().enterEvent(e)

    def leaveEvent(self, e):
        self._hovered = False
        self.update()
        super().leaveEvent(e)

    def paintEvent(self, event):
        p = QPainter(self)
        p.setRenderHint(QPainter.Antialiasing)
        r = BALL_R
        cx = cy = r

        # body: radial gradient (hover only darkens color, no scale)
        base = self._color.darker(110) if self._hovered else self._color
        grad = QRadialGradient(cx - r//3, cy - r//3, r)
        grad.setColorAt(0, QColor(min(255, base.red()+40), min(255, base.green()+40),
                                   min(255, base.blue()+40), base.alpha()))
        grad.setColorAt(0.8, base)
        grad.setColorAt(1, QColor(base.red()-20, base.green()-20, base.blue()-20, base.alpha()))
        p.setBrush(grad)
        border = self._border if self._border else QColor(base.red()-30, base.green()-30, base.blue()-30, base.alpha())
        p.setPen(QPen(border, 1))
        off = (self.width() - r * 2) // 2
        p.drawEllipse(off, off, r * 2, r * 2)

        # highlight
        hl = QRadialGradient(cx - r//3, cy - r//3, r//2)
        hl.setColorAt(0, QColor(255, 255, 255, 60))
        hl.setColorAt(1, QColor(255, 255, 255, 0))
        p.setBrush(hl)
        p.setPen(Qt.NoPen)
        p.drawEllipse(off, off, r * 2, r * 2)

        # text
        if self._text:
            p.setPen(C_TEXT)
            f = p.font()
            f.setPixelSize(12 if len(self._text) > 2 else 14)
            f.setBold(False)
            f.setWeight(QFont.Medium)
            p.setFont(f)
            p.drawText(self.rect(), Qt.AlignCenter, self._text)

        # BLE connected indicator: green dot at ball center
        if self._connected:
            s = 8
            cx2, cy2 = self.width() // 2, self.height() // 2
            p.setBrush(QColor(0, 200, 0, 220))
            p.setPen(Qt.NoPen)
            p.drawEllipse(cx2 - s // 2, cy2 - s // 2, s, s)


class MiniEditor(QWidget):
    """非模态编辑窗口 — 点击悬浮球文字时弹出，支持直接编辑"""
    editor_accepted = pyqtSignal(str)

    def __init__(self, text, parent=None):
        super().__init__(None)  # 独立窗口，非父窗口子控件
        self.setWindowTitle("编辑文字")
        self.setWindowFlags(
            Qt.WindowStaysOnTopHint | Qt.FramelessWindowHint
            | Qt.Tool)
        self.setAttribute(Qt.WA_TranslucentBackground)
        self.setAttribute(Qt.WA_DeleteOnClose)
        self.setStyleSheet("""
            MiniEditor {
                background: rgba(255, 255, 255, 0.92);
                border: 1px solid rgba(150, 200, 230, 0.55);
                border-radius: 10px;
            }
        """)

        layout = QVBoxLayout(self)
        layout.setContentsMargins(10, 10, 10, 8)
        layout.setSpacing(6)

        self._edit = QTextEdit(text)
        self._edit.setPlaceholderText("直接修改识别结果…")
        self._edit.setStyleSheet("""
            QTextEdit {
                background: rgba(255,255,255,0.85);
                color: #1a1a1a;
                font-size: 14px;
                border: 1px solid rgba(135,206,235,0.4);
                border-radius: 6px;
                padding: 8px 10px;
            }
            QTextEdit:focus {
                border-color: rgba(135,206,235,0.8);
                background: rgba(255,255,255,0.95);
            }
        """)
        layout.addWidget(self._edit, 1)

        btn_lay = QHBoxLayout()
        btn_lay.setSpacing(6)

        self._status_label = QLabel("Ctrl+Enter 确定  |  Esc 取消")
        self._status_label.setStyleSheet("color: #999; font-size: 11px;")

        ok_btn = QPushButton("确定并复制")
        ok_btn.setFixedSize(100, 30)
        ok_btn.setStyleSheet("""
            QPushButton {
                background: rgba(80, 180, 230, 0.8);
                color: white;
                border: none;
                border-radius: 6px;
                font-size: 13px;
                font-weight: bold;
            }
            QPushButton:hover {
                background: rgba(60, 160, 220, 0.9);
            }
            QPushButton:pressed {
                background: rgba(40, 140, 200, 1.0);
            }
        """)
        ok_btn.clicked.connect(self._accept)

        cancel_btn = QPushButton("取消")
        cancel_btn.setFixedSize(70, 30)
        cancel_btn.setStyleSheet("""
            QPushButton {
                background: rgba(200, 200, 200, 0.6);
                color: #333;
                border: none;
                border-radius: 6px;
                font-size: 13px;
            }
            QPushButton:hover {
                background: rgba(180, 180, 180, 0.7);
            }
        """)
        cancel_btn.clicked.connect(self.close)

        btn_lay.addWidget(self._status_label)
        btn_lay.addStretch(1)
        btn_lay.addWidget(ok_btn)
        btn_lay.addWidget(cancel_btn)
        layout.addLayout(btn_lay)

        # 快捷键
        QShortcut(QKeySequence("Ctrl+Return"), self).activated.connect(self._accept)
        QShortcut(QKeySequence("Escape"), self).activated.connect(self.close)

        self._edit.setFocus()

    def set_geometry(self, x, y, w, h):
        self.setGeometry(x, y, w, h)

    def _accept(self):
        text = self._edit.toPlainText()
        self.editor_accepted.emit(text)
        self.close()


class _FuncBtn(QPushButton):
    def __init__(self, text, parent):
        super().__init__(text, parent)
        self.setFixedSize(44, 28)
        self.setCursor(Qt.PointingHandCursor)
        self._hover = False
        self.setMouseTracking(True)
        self.setStyleSheet("background:transparent;border:none;color:" + C_TEXT.name() + ";font-size:11px;")

    def enterEvent(self, e):
        self._hover = True
        self.update()
        super().enterEvent(e)

    def leaveEvent(self, e):
        self._hover = False
        self.update()
        super().leaveEvent(e)

    def paintEvent(self, event):
        p = QPainter(self)
        p.setRenderHint(QPainter.Antialiasing)
        base = QColor(160, 215, 242, 200) if not self._hover else QColor(135, 206, 235, 220)
        # pill shape
        p.setBrush(base)
        p.setPen(QPen(QColor(155, 210, 240, 80), 1))
        p.drawRoundedRect(2, 2, self.width() - 4, self.height() - 4, 8, 8)
        # text
        p.setPen(C_TEXT)
        f = p.font()
        f.setPixelSize(12)
        p.setFont(f)
        p.drawText(self.rect(), Qt.AlignCenter, self.text())
