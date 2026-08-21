"""VoiceStick 配置管理（JSON）"""
import json
from pathlib import Path
from dataclasses import dataclass, field, asdict


@dataclass
class AppConfig:
    paired_device_ids: list[str] = field(default_factory=list)
    last_connected_address: str = ""
    last_connected_name: str = ""
    asr_server_url: str = "wss://openspeech.bytedance.com/api/v3/sauc/bigmodel_async"
    asr_api_key: str = ""
    language: str = "zh-CN"
    paste_on_final: bool = True
    press_enter_after_paste: bool = False
    output_mode: str = "clipboard"  # "clipboard" | "direct"
    interaction_mode: str = "hold_to_talk"
    overlay_theme: str = "auto"
    subtitle_enabled: bool = True
    subtitle_position: str = "bottom"
    brightness: int = 50
    debug_audio: bool = False
    debug_audio_dir: str = ""
    # LLM 翻译
    enable_translation: bool = False
    translation_target: str = "English"
    llm_api_key: str = ""
    llm_base_url: str = "https://api.openai.com/v1"
    llm_model: str = "gpt-4o-mini"
    # LLM 润色
    enable_polish: bool = False
    polish_prompt: str = (
        "你是一个中文润色助手。"
        "将用户输入的口语化文本整理为书面表达，修正语病、冗余和不自然之处，"
        "使表达更清晰流畅。保留原意和风格。"
        "只返回润色后的文本，不要解释。"
    )
    polish_position: str = "before_translate"  # before_translate | after_translate
    # 悬浮球位置
    floatball_x: int = -1
    floatball_y: int = -1

    CONFIG_PATH = Path.home() / ".voicestick" / "config.json"

    @classmethod
    def load(cls) -> "AppConfig":
        path = cls.CONFIG_PATH
        if not path.exists():
            return cls()
        try:
            data = json.loads(path.read_text(encoding="utf-8"))
            return cls(**{k: v for k, v in data.items() if k in cls.__dataclass_fields__})
        except Exception:
            return cls()

    def save(self):
        path = self.CONFIG_PATH
        path.parent.mkdir(parents=True, exist_ok=True)
        data = {k: v for k, v in asdict(self).items() if k != "CONFIG_PATH"}
        path.write_text(json.dumps(data, ensure_ascii=False, indent=2), encoding="utf-8")
