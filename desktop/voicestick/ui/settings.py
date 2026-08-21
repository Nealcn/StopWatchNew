"""设置对话框 — 分组模块化排版"""
from PyQt5.QtWidgets import (
    QDialog, QVBoxLayout, QHBoxLayout,
    QLineEdit, QTextEdit, QCheckBox, QPushButton,
    QLabel, QGroupBox, QWidget, QComboBox,
)
from PyQt5.QtCore import Qt
from ..config import AppConfig


class _PasswordLine(QWidget):
    """带显示/隐藏按钮的密码输入框"""
    def __init__(self, text="", placeholder=""):
        super().__init__()
        self._visible = False
        layout = QHBoxLayout(self)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.setSpacing(4)
        self._edit = QLineEdit(text)
        self._edit.setPlaceholderText(placeholder)
        self._edit.setEchoMode(QLineEdit.Password)
        self._btn = QPushButton("👁")
        self._btn.setFixedSize(28, 28)
        self._btn.setToolTip("显示/隐藏")
        self._btn.clicked.connect(self._toggle)
        layout.addWidget(self._edit)
        layout.addWidget(self._btn)

    def _toggle(self):
        self._visible = not self._visible
        self._edit.setEchoMode(QLineEdit.Normal if self._visible else QLineEdit.Password)

    def text(self):
        return self._edit.text()

    def setText(self, t):
        self._edit.setText(t)

    def clear(self):
        self._edit.clear()


class SettingsDialog(QDialog):
    def __init__(self, config: AppConfig, parent=None):
        super().__init__(parent)
        self._config = config
        self._changed = False
        self.setWindowTitle("VoiceStick 设置")
        self.setFixedSize(560, 700)
        self._setup_ui()

    # ---- 统一样式 ----

    STYLE = """
        QGroupBox {
            font-weight: bold;
            font-size: 13px;
            padding-top: 8px;
            margin-top: 2px;
        }
        QGroupBox::title {
            subcontrol-origin: margin;
            left: 10px;
            padding: 0 4px;
        }
        QLineEdit {
            min-height: 26px;
            font-size: 13px;
            padding: 1px 8px;
        }
        QTextEdit {
            min-height: 54px;
            font-size: 13px;
            padding: 3px 8px;
        }
        QComboBox {
            min-height: 26px;
            font-size: 13px;
            padding: 1px 6px;
        }
    """

    # ---- 统一表单行：左侧标签固定宽度 ----
    @staticmethod
    def _row(widget, label=""):
        """标准表单行：标签 + 控件水平排列"""
        row = QHBoxLayout()
        row.setContentsMargins(0, 0, 0, 0)
        row.setSpacing(8)
        if label:
            lbl = QLabel(label)
            lbl.setFixedWidth(100)
            lbl.setAlignment(Qt.AlignRight | Qt.AlignVCenter)
            row.addWidget(lbl)
        row.addWidget(widget, 1)
        return row

    @staticmethod
    def _row_check(widget):
        """复选框行：左对齐"""
        row = QHBoxLayout()
        row.setContentsMargins(0, 0, 0, 0)
        row.addSpacing(108)  # 对齐标签宽度
        row.addWidget(widget)
        row.addStretch()
        return row

    # ---- 构建 ----

    def _setup_ui(self):
        self.setStyleSheet(self.STYLE)

        outer = QVBoxLayout(self)
        outer.setSpacing(4)
        outer.setContentsMargins(10, 4, 10, 6)

        # ========== 1. 语音识别 ==========
        asr_box = QGroupBox("语音识别")
        asr_layout = QVBoxLayout(asr_box)
        asr_layout.setSpacing(10)
        asr_layout.setContentsMargins(10, 12, 10, 10)
        self._asr_url = QLineEdit(self._config.asr_server_url)
        self._asr_url.setPlaceholderText("wss://openspeech.bytedance.com/api/v3/sauc/bigmodel")
        asr_layout.addLayout(self._row(self._asr_url, "ASR 服务器："))
        self._asr_key = _PasswordLine(self._config.asr_api_key, "火山引擎控制台的 APP Key")
        asr_layout.addLayout(self._row(self._asr_key, "API Key："))
        outer.addWidget(asr_box)

        # ========== 2. 输入配置 ==========
        inp_box = QGroupBox("输入配置")
        inp_layout = QVBoxLayout(inp_box)
        inp_layout.setSpacing(10)
        inp_layout.setContentsMargins(10, 12, 10, 10)
        self._paste_check = QCheckBox("自动复制文字")
        self._paste_check.setChecked(self._config.paste_on_final)
        self._paste_check.setToolTip("识别完成后自动复制到剪贴板，你手动 Ctrl+V 粘贴")
        inp_layout.addLayout(self._row_check(self._paste_check))
        self._paired_ids = QLineEdit(", ".join(self._config.paired_device_ids))
        self._paired_ids.setPlaceholderText("例如: E3F6, A1B2")
        inp_layout.addLayout(self._row(self._paired_ids, "已配对设备："))
        outer.addWidget(inp_box)

        # ========== 3. LLM 大模型 ==========
        llm_box = QGroupBox("LLM 大模型")
        llm_layout = QVBoxLayout(llm_box)
        llm_layout.setSpacing(10)
        llm_layout.setContentsMargins(10, 12, 10, 10)
        self._llm_url = QLineEdit(self._config.llm_base_url)
        self._llm_url.setPlaceholderText("https://api.openai.com/v1")
        llm_layout.addLayout(self._row(self._llm_url, "API 地址："))
        self._llm_key = _PasswordLine(self._config.llm_api_key, "sk-...")
        llm_layout.addLayout(self._row(self._llm_key, "API Key："))
        self._llm_model = QLineEdit(self._config.llm_model)
        self._llm_model.setPlaceholderText("gpt-4o-mini")
        llm_layout.addLayout(self._row(self._llm_model, "模型："))
        outer.addWidget(llm_box)

        # ========== 4. LLM 润色 ==========
        polish_box = QGroupBox("LLM 润色")
        polish_layout = QVBoxLayout(polish_box)
        polish_layout.setSpacing(10)
        polish_layout.setContentsMargins(10, 12, 10, 10)
        self._polish_check = QCheckBox("启用 LLM 润色")
        self._polish_check.setChecked(self._config.enable_polish)
        polish_layout.addLayout(self._row_check(self._polish_check))
        self._polish_prompt = QTextEdit()
        self._polish_prompt.setPlainText(self._config.polish_prompt)
        self._polish_prompt.setPlaceholderText("你是一个中文润色助手...")
        polish_layout.addLayout(self._row(self._polish_prompt, "提示词："))
        outer.addWidget(polish_box)

        # ========== 5. 翻译 ==========
        trans_box = QGroupBox("翻译")
        trans_layout = QVBoxLayout(trans_box)
        trans_layout.setSpacing(10)
        trans_layout.setContentsMargins(10, 12, 10, 10)
        self._trans_check = QCheckBox("启用 LLM 翻译")
        self._trans_check.setChecked(self._config.enable_translation)
        trans_layout.addLayout(self._row_check(self._trans_check))
        self._trans_target = QLineEdit(self._config.translation_target)
        self._trans_target.setPlaceholderText("English, Japanese, 日本語...")
        trans_layout.addLayout(self._row(self._trans_target, "目标语言："))
        outer.addWidget(trans_box)

        # ========== 保存/取消 ==========
        btn_row = QHBoxLayout()
        btn_row.setContentsMargins(0, 6, 0, 0)
        btn_row.addStretch()
        save_btn = QPushButton("保存")
        save_btn.setFixedWidth(80)
        save_btn.clicked.connect(self._save)
        cancel_btn = QPushButton("取消")
        cancel_btn.setFixedWidth(80)
        cancel_btn.clicked.connect(self.reject)
        btn_row.addWidget(save_btn)
        btn_row.addSpacing(10)
        btn_row.addWidget(cancel_btn)
        outer.addLayout(btn_row)

    # ---- 保存 ----

    def _save(self):
        ids_text = self._paired_ids.text().strip()
        if ids_text:
            self._config.paired_device_ids = [
                pid.strip() for pid in ids_text.replace("，", ",").split(",") if pid.strip()
            ]
        else:
            self._config.paired_device_ids = []
        self._config.asr_server_url = self._asr_url.text().strip()
        self._config.asr_api_key = self._asr_key.text().strip()
        self._config.paste_on_final = self._paste_check.isChecked()
        self._config.llm_base_url = self._llm_url.text().strip()
        self._config.llm_api_key = self._llm_key.text().strip()
        self._config.llm_model = self._llm_model.text().strip()
        self._config.enable_polish = self._polish_check.isChecked()
        self._config.polish_prompt = self._polish_prompt.toPlainText().strip()
        self._config.enable_translation = self._trans_check.isChecked()
        self._config.translation_target = self._trans_target.text().strip()
        self._config.save()
        self._changed = True
        self.accept()

    @property
    def changed(self) -> bool:
        return self._changed
