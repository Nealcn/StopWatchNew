"""LLM 翻译客户端 — 调用 OpenAI 兼容 API 翻译 ASR 识别文本"""
import asyncio
import json
import logging
from typing import Optional
from dataclasses import dataclass

import aiohttp

logger = logging.getLogger(__name__)


@dataclass
class LLMTranslationResult:
    text: str
    error: Optional[str] = None


class LLMTranslationClient:
    """使用 LLM 实时翻译 ASR 识别文本"""

    def __init__(self, api_key: str = "", base_url: str = "",
                 model: str = "gpt-4o-mini", target_language: str = "English"):
        self._api_key = api_key
        self._base_url = base_url.rstrip("/")
        self._model = model
        self._target_language = target_language
        self._session: Optional[aiohttp.ClientSession] = None

    def update_config(self, api_key: str, base_url: str, model: str, target_language: str):
        self._api_key = api_key
        self._base_url = base_url.rstrip("/")
        self._model = model
        self._target_language = target_language

    @property
    def is_configured(self) -> bool:
        return bool(self._api_key and self._base_url)

    async def complete(self, system_prompt: str, text: str) -> LLMTranslationResult:
        """通用 LLM 调用：给定 system prompt + 用户文本，返回生成结果"""
        if not self._api_key:
            return LLMTranslationResult("", error="API Key 未设置")
        if not self._base_url:
            return LLMTranslationResult("", error="API 地址未设置")

        url = self._chat_completions_url()
        if not url:
            return LLMTranslationResult("", error="无效的 API 地址")

        headers = {
            "Authorization": f"Bearer {self._api_key}",
            "Content-Type": "application/json",
        }

        payload = {
            "model": self._model,
            "temperature": 0,
            "messages": [
                {"role": "system", "content": system_prompt},
                {"role": "user", "content": text},
            ],
        }

        try:
            # 每次调用都创建新会话，避免跨事件循环复用导致 Timeout 异常
            if self._session:
                await self._session.close()
            self._session = aiohttp.ClientSession()
            resp = await asyncio.wait_for(
                self._session.post(url, json=payload, headers=headers), timeout=10
            )
            if resp.status != 200:
                body = await resp.text()
                resp.release()
                logger.warning("LLM API 返回 %d: %s", resp.status, body[:200])
                return LLMTranslationResult("", error=f"API 返回 {resp.status}")
            data = await resp.json()
            resp.release()
            choices = data.get("choices", [])
            if not choices:
                return LLMTranslationResult("", error="LLM 返回空结果")
            message = choices[0].get("message", {})
            content = message.get("content", "").strip()
            return LLMTranslationResult(text=content)
        except asyncio.TimeoutError:
            return LLMTranslationResult("", error="LLM 超时")
        except Exception as e:
            logger.warning("LLM 调用失败: %s", e)
            return LLMTranslationResult("", error=f"LLM 失败: {e}")

    async def translate(self, text: str, hotwords: list[str] = None) -> LLMTranslationResult:
        """翻译文本"""
        system_prompt = self._build_system_prompt(hotwords or [])
        result = await self.complete(system_prompt, text)
        if not result.error:
            logger.info("LLM 翻译: '%s' → '%s'", text[:50], result.text[:50])
        return result

    async def polish(self, text: str, system_prompt: str) -> LLMTranslationResult:
        """润色文本，使用自定义 system_prompt"""
        result = await self.complete(system_prompt, text)
        if not result.error:
            logger.info("LLM 润色: '%s' → '%s'", text[:50], result.text[:50])
        return result

    def _chat_completions_url(self) -> Optional[str]:
        base = self._base_url.rstrip("/ \n\r\t")
        if not base:
            return None
        if base.endswith("/chat/completions"):
            return base
        return f"{base}/chat/completions"

    def _build_system_prompt(self, hotwords: list[str]) -> str:
        prompt = (
            "You are a real-time speech translator.\n"
            f"Translate the user's text into {self._target_language}.\n"
            "Detect the source language automatically.\n"
            "Return only the translated text, with no explanations, quotes, prefixes, alternatives, or markdown.\n"
            "The text may come from live speech recognition and may contain minor recognition errors; "
            "infer the intended meaning when it is clear."
        )
        if hotwords:
            terms = [w.strip() for w in hotwords if w.strip()]
            if terms:
                prompt += "\n\nImportant terms that may appear:\n"
                prompt += "\n".join(f"- {t}" for t in terms)
        return prompt

    async def close(self):
        if self._session:
            await self._session.close()
            self._session = None

