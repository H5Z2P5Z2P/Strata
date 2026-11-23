#!/usr/bin/env python3
"""Controlled prefill & decode micro-benchmark against a running Strata OpenAI server."""
import argparse, json, sys, time, urllib.request


def post_chat(url: str, payload: dict, timeout: int = 300) -> dict:
    data = json.dumps(payload).encode("utf-8")
    req = urllib.request.Request(
        url, data=data, headers={"Content-Type": "application/json", "Authorization": "Bearer bench"}
    )
    t0 = time.monotonic()
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        body = json.loads(resp.read().decode("utf-8"))
    dt = time.monotonic() - t0
    body["_wall_s"] = dt
    return body


def make_long_prompt(target_words: int, salt: str) -> str:
    para = (
        f"[SALT:{salt}] System architecture analysis of distributed GPU inference engines, "
        "covering PCIe DMA staging rings, ROCm HIP wave32 scheduling, KV cache quantization, "
        "sparse attention block selection, Mixture-of-Experts routing, and host memory locking. "
    )
    chunks = []
    for i in range(max(1, target_words // 30)):
        chunks.append(f"Section {i:04d}: " + para)
    chunks.append(
        f"\nKey token to recall verbatim: KEY-{salt}-7900XTX.\n"
        "Reply with ONLY the key token above and nothing else."
    )
    return "\n".join(chunks)


def run_case(url: str, model: str, name: str, prompt: str, max_tokens: int, temp: float, seed: int):
    payload = {
        "model": model,
        "messages": [{"role": "user", "content": prompt}],
        "max_tokens": max_tokens,
        "temperature": temp,
        "seed": seed,
        "reasoning_effort": "none",
    }
    res = post_chat(url, payload)
    usage = res.get("usage", {})
    msg = res["choices"][0]["message"]
    text = (msg.get("content") or msg.get("reasoning_content") or "").strip().replace("\n", " ")
    pt = usage.get("prompt_tokens", 0)
    ct = usage.get("completion_tokens", 0)
    print(
        f"[{name}] prompt={pt} out={ct} wall={res['_wall_s']:.2f}s "
        f"text={text[:90]!r}",
        flush=True,
    )
    return {"name": name, "prompt_tokens": pt, "completion_tokens": ct, "wall_s": round(res["_wall_s"], 3), "text": text[:160]}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=18081)
    ap.add_argument("--model", default="qwen38-flash-next-iq2_xs")
    ap.add_argument("--salt", default="run1")
    args = ap.parse_args()
    url = f"http://127.0.0.1:{args.port}/v1/chat/completions"

    out = []
    # 1. Warmup
    out.append(run_case(url, args.model, "warmup", "Reply OK.", 16, 0.0, 42))
    # 2. Cold ~16K prompt prefill
    p16k = make_long_prompt(11500, args.salt + "-16k")
    out.append(run_case(url, args.model, "prefill_16k", p16k, 24, 0.0, 42))
    # 3. Greedy code decode (256 tokens)
    out.append(
        run_case(
            url,
            args.model,
            "decode_code_greedy",
            f"[{args.salt}-code] Write a clean C++20 lock-free SPSC ring buffer class with comments and unit test.",
            256,
            0.0,
            42,
        )
    )
    # 4. Greedy Chinese prose decode (256 tokens)
    out.append(
        run_case(
            url,
            args.model,
            "decode_zh_greedy",
            f"[{args.salt}-zh] 请用中文写一段关于在单张消费级显卡上运行大语言模型工程优化的技术短文，约350字。",
            256,
            0.0,
            42,
        )
    )
    # 5. Sampled decode (temp=0.7) to test STRATA_SPEC_COUPLED
    out.append(
        run_case(
            url,
            args.model,
            "decode_zh_sampled",
            f"[{args.salt}-samp] 请用中文通俗解释什么是混合专家模型（MoE）以及为什么需要显存与内存协同。",
            256,
            0.7,
            42,
        )
    )
    print("SUMMARY_JSON:" + json.dumps(out, ensure_ascii=False))


if __name__ == "__main__":
    main()
