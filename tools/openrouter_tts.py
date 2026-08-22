#!/usr/bin/env python3
"""Generate raw audio with OpenRouter's OpenAI-compatible TTS endpoint.

The API key is read from OPENROUTER_API_KEY and is never written to disk.
If the direct request cannot connect, the request is retried through the
local proxy at 127.0.0.1:17890 (or OPENROUTER_PROXY when set).
"""

from __future__ import annotations

import argparse
import json
import os
import sys
from pathlib import Path
from urllib.error import HTTPError, URLError
from urllib.request import ProxyHandler, Request, build_opener

ENDPOINT = "https://openrouter.ai/api/v1/audio/speech"
DEFAULT_MODEL = "fish-audio/s2.1-pro-free:free"


def request_audio(
    *,
    api_key: str,
    text: str,
    model: str,
    voice: str | None,
    response_format: str,
    proxy: str | None,
    instructions: str | None,
) -> tuple[bytes, str | None]:
    payload = {
        "model": model,
        "input": text,
        "response_format": response_format,
    }
    if voice:
        payload["voice"] = voice
    if instructions:
        payload["instructions"] = instructions
    request = Request(
        ENDPOINT,
        data=json.dumps(payload, ensure_ascii=False).encode("utf-8"),
        headers={
            "Authorization": f"Bearer {api_key}",
            "Content-Type": "application/json",
            "HTTP-Referer": "https://github.com/sensevoice-desk",
            "X-Title": "SenseVoice Desk",
        },
        method="POST",
    )
    opener = build_opener(ProxyHandler({"http": proxy, "https": proxy})) if proxy else build_opener()
    with opener.open(request, timeout=180) as response:
        return response.read(), response.headers.get("X-Generation-Id")


def generate(args: argparse.Namespace) -> int:
    api_key = os.environ.get("OPENROUTER_API_KEY")
    if not api_key:
        print("OPENROUTER_API_KEY is not set", file=sys.stderr)
        return 2

    text = Path(args.text_file).read_text(encoding="utf-8") if args.text_file else args.text
    if not text or not text.strip():
        print("Input text is empty", file=sys.stderr)
        return 2

    fallback_proxy = args.proxy or os.environ.get("OPENROUTER_PROXY", "http://127.0.0.1:17890")
    try:
        audio, generation_id = request_audio(
            api_key=api_key,
            text=text,
            model=args.model,
            voice=args.voice,
            response_format=args.response_format,
            proxy=None,
            instructions=args.instructions,
        )
        route = "direct"
    except (HTTPError, URLError, TimeoutError, OSError) as direct_error:
        if isinstance(direct_error, HTTPError) and direct_error.code < 500:
            detail = direct_error.read().decode("utf-8", errors="replace")
            print(f"OpenRouter rejected the request ({direct_error.code}): {detail}", file=sys.stderr)
            return 1
        try:
            audio, generation_id = request_audio(
                api_key=api_key,
                text=text,
                model=args.model,
                voice=args.voice,
                response_format=args.response_format,
                proxy=fallback_proxy,
                instructions=args.instructions,
            )
            route = f"proxy {fallback_proxy}"
        except (HTTPError, URLError, TimeoutError, OSError) as proxy_error:
            if isinstance(proxy_error, HTTPError):
                detail = proxy_error.read().decode("utf-8", errors="replace")
                print(f"OpenRouter rejected the request ({proxy_error.code}): {detail}", file=sys.stderr)
            else:
                print(f"OpenRouter connection failed: {proxy_error}", file=sys.stderr)
            return 1

    output = Path(args.output)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_bytes(audio)
    print(f"saved {output} ({len(audio)} bytes, route={route}, generation={generation_id or 'unknown'})")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument("--text", help="Text to synthesize")
    source.add_argument("--text-file", help="UTF-8 text file to synthesize")
    parser.add_argument("--output", required=True, help="Output audio path")
    parser.add_argument("--model", default=DEFAULT_MODEL)
    parser.add_argument("--voice", help="Optional provider voice; omit it to use the model's default voice")
    parser.add_argument("--response-format", choices=("mp3", "pcm"), default="mp3")
    parser.add_argument("--instructions", help="Optional delivery/style instruction for the TTS model")
    parser.add_argument("--proxy", help="HTTP proxy, defaulting to OPENROUTER_PROXY or 127.0.0.1:17890 on fallback")
    return generate(parser.parse_args())


if __name__ == "__main__":
    raise SystemExit(main())
