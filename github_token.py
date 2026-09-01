#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
github_token — GitHub Personal Access Token 管理工具
================================================================
统一管理 GitHub PAT 的生成指引 / 检测 / 测试 / 保存 / 诊断,
避免每个项目 (godot_ui_linux, BV_WorkSpace, ai_tools) 重复处理
"push 失败 / token 失效" 错误.

用法 (CLI):
    github-token --check             # 检当前 token 是否有效
    github-token --diagnose          # 完整诊断 (含所有 repo 访问权限)
    github-token --set <TOKEN>       # 保存新 token 到 ~/.git-credentials
    github-token --clear             # 清掉旧 token
    github-token --setup             # 打印生成新 token 的 step-by-step 指引
    github-token --url               # 只打印 GitHub 生成 token 的 URL
    github-token --info              # 显示 token 来源 / 状态

Token 来源优先级 (从高到低):
    1. --set <TOKEN> 命令行参数
    2. $GITHUB_TOKEN / $GH_TOKEN 环境变量
    3. ~/.git-credentials (git credential.helper=store)
    4. git config credential.username / credential.helper (不常用)

GUI: github_token_gui.py
集成: ac ght / ac ght-cli / ac github-token

依赖: curl (git 也行, 但 API 用 curl 更快)
"""
import os
import sys
import json
import shutil
import subprocess
import argparse
import re
from pathlib import Path
from urllib.parse import urlparse
from dataclasses import dataclass, field
from datetime import datetime, timezone
from typing import Optional

GITHUB_API = "https://api.github.com"
GITHUB_WEB = "https://github.com"

# 已知 repo (本项目) — 诊断时默认查这些
KNOWN_REPOS = [
    "happylihappyli/ai_tools",
    "happylihappyli/godot_ui_linux",
    "happylihappyli/BV_WorkSpace_Linux",
]

# 生成 token 的 URL (用户去这里创建)
# classic: https://github.com/settings/tokens/new
# fine-grained: https://github.com/settings/personal-access-tokens/new
SETUP_URLS = {
    "classic": f"{GITHUB_WEB}/settings/tokens/new",
    "fine-grained": f"{GITHUB_WEB}/settings/personal-access-tokens/new",
}


@dataclass
class TokenStatus:
    """Token 状态"""
    token: str = ""
    source: str = ""           # 来自哪里 (cli/env/credentials/none)
    valid: bool = False        # 是否有效
    user: str = ""             # GitHub 用户名
    scopes: list = field(default_factory=list)
    is_fine_grained: bool = False
    expires_at: Optional[str] = None   # ISO 8601 or None
    error: str = ""            # 错误信息 (无效时填)
    rate_remaining: int = -1  # API rate limit 剩余
    repos: dict = field(default_factory=dict)  # owner/repo -> {permissions, private, accessible}


def curl_json(url: str, token: str = "", method: str = "GET",
              data: str = "") -> tuple[int, dict | str]:
    """用 curl 调 GitHub API. 返回 (http_code, body_json_or_text)."""
    cmd = ["curl", "-sS", "-X", method, "-w", "\n%{http_code}",
           "-H", "Accept: application/vnd.github+json",
           "-H", "X-GitHub-Api-Version: 2022-11-28",
           "-H", "User-Agent: ai_tools-github_token-tool"]
    if token:
        cmd += ["-H", f"Authorization: token {token}"]
    if data:
        cmd += ["-H", "Content-Type: application/json", "-d", data]
    cmd.append(url)
    try:
        out = subprocess.run(cmd, capture_output=True, text=True, timeout=15)
        text = out.stdout
        parts = text.rsplit("\n", 1)
        if len(parts) == 2 and parts[1].isdigit():
            code = int(parts[1])
            body = parts[0]
        else:
            return 0, text
        try:
            return code, json.loads(body)
        except Exception:
            return code, body
    except subprocess.TimeoutExpired:
        return 0, "timeout"
    except Exception as e:
        return 0, str(e)


def find_token_from_credentials() -> tuple[str, str]:
    """从 ~/.git-credentials 找 token.
    返回 (token, source) 或 ("", "")
    """
    cred_paths = [
        Path.home() / ".git-credentials",
        Path(os.environ.get("XDG_CONFIG_HOME", Path.home() / ".config")) / "git" / "credentials",
    ]
    for p in cred_paths:
        if not p.exists():
            continue
        try:
            for line in p.read_text(encoding="utf-8", errors="ignore").splitlines():
                line = line.strip()
                if not line or line.startswith("#"):
                    continue
                # https://oauth2:TOKEN@github.com
                m = re.search(r"oauth2:([^@]+)@", line)
                if m:
                    return m.group(1), f"file:{p}"
                # https://TOKEN:x-oauth-basic@github.com (git over https with basic auth)
                m = re.search(r"://([^:]+):x-oauth-basic@", line)
                if m:
                    return m.group(1), f"file:{p}"
                # https://x-access-token:TOKEN@github.com (新格式)
                m = re.search(r"x-access-token:([^@]+)@", line)
                if m:
                    return m.group(1), f"file:{p}"
        except Exception:
            continue
    return "", ""


def find_token() -> tuple[str, str]:
    """按优先级找 token. 返回 (token, source)."""
    # 1. 环境变量
    for var in ("GITHUB_TOKEN", "GH_TOKEN", "GITHUB_ACCESS_TOKEN"):
        v = os.environ.get(var, "").strip()
        if v:
            return v, f"env:{var}"
    # 2. ~/.git-credentials
    tok, src = find_token_from_credentials()
    if tok:
        return tok, src
    return "", ""


def check_token(token: str) -> TokenStatus:
    """调 GitHub API 全面检查 token 状态."""
    st = TokenStatus(token=token)
    if not token:
        st.error = "token 为空"
        return st
    code, body = curl_json(f"{GITHUB_API}/user", token)
    if code == 200 and isinstance(body, dict):
        st.valid = True
        st.user = body.get("login", "")
        # classic token 才有 X-OAuth-Scopes header, fine-grained 走 /user/permissions
        # 这里通过 /repos/... 看是否能访问
        st.is_fine_grained = body.get("type") == "Bot" or token.startswith("github_pat_")
    elif code == 401:
        st.error = "token 无效或已过期 (HTTP 401)"
    elif code == 403:
        st.error = "token 权限不足或被 rate-limited (HTTP 403)"
    else:
        st.error = f"未知错误 HTTP {code}: {str(body)[:200]}"
    # rate limit
    code2, _ = curl_json(f"{GITHUB_API}/rate_limit", token)
    if code2 == 200:
        rc, _ = curl_json(f"{GITHUB_API}/rate_limit", token)
        # 再拿一次拿 header 信息 (body 里有 rate info)
        try:
            cmd = ["curl", "-sS", "-I", "-H", f"Authorization: token {token}",
                   f"{GITHUB_API}/rate_limit"]
            out = subprocess.run(cmd, capture_output=True, text=True, timeout=10)
            for ln in out.stdout.splitlines():
                if "x-ratelimit-remaining:" in ln.lower():
                    st.rate_remaining = int(ln.split(":")[1].strip())
                if "x-ratelimit-reset:" in ln.lower():
                    pass
        except Exception:
            pass
    return st


def check_repo(token: str, owner_repo: str) -> dict:
    """查 token 对某 repo 的访问权限. 返回 dict."""
    code, body = curl_json(f"{GITHUB_API}/repos/{owner_repo}", token)
    if code == 200 and isinstance(body, dict):
        return {
            "accessible": True,
            "http": code,
            "private": body.get("private", False),
            "permissions": body.get("permissions", {}),
            "description": body.get("description", ""),
        }
    elif code == 404:
        return {"accessible": False, "http": 404, "error": "not found (private or no access)"}
    elif code == 403:
        return {"accessible": False, "http": 403, "error": "forbidden (no permission)"}
    else:
        return {"accessible": False, "http": code, "error": str(body)[:200]}


def diagnose(token: str = "", verbose: bool = True) -> TokenStatus:
    """完整诊断: token 状态 + 已知 repo 访问."""
    if not token:
        token, source = find_token()
    else:
        source = "cli"
    st = check_token(token) if token else TokenStatus()
    st.source = source or "未设置"
    if token and st.valid:
        # 查所有已知 repo
        for r in KNOWN_REPOS:
            st.repos[r] = check_repo(token, r)
    if verbose:
        print_status(st)
    return st


def print_status(st: TokenStatus) -> None:
    """打印 token 状态 (CLI 输出)."""
    print("=" * 70)
    print(f"Token 状态")
    print("=" * 70)
    print(f"  来源:      {st.source or '未设置'}")
    if st.token:
        masked = st.token[:4] + "*" * max(0, len(st.token) - 8) + st.token[-4:] if len(st.token) > 8 else "***"
        print(f"  Token:     {masked}  (长度 {len(st.token)})")
        print(f"  类型:      {'fine-grained' if st.is_fine_grained else 'classic/other'}")
        if st.valid:
            print(f"  状态:      ✓ 有效")
            print(f"  用户:      {st.user}")
            if st.rate_remaining >= 0:
                print(f"  Rate:      {st.rate_remaining} 次剩余")
        else:
            print(f"  状态:      ✗ 无效")
            print(f"  错误:      {st.error}")
    else:
        print(f"  Token:     (未设置)")
    if st.repos:
        print("-" * 70)
        print("Repo 访问权限:")
        for r, info in st.repos.items():
            if info.get("accessible"):
                perms = info.get("permissions", {})
                perm_str = ",".join([k for k, v in perms.items() if v]) or "(无)"
                priv = "🔒 私有" if info.get("private") else "🌐 公开"
                print(f"  ✓ {r:<35} {priv}  权限: {perm_str}")
            else:
                print(f"  ✗ {r:<35} HTTP {info.get('http', '?')}: {info.get('error', '')}")
    print("=" * 70)


def get_setup_instructions() -> str:
    """返回生成新 token 的 step-by-step 指引 (中文)."""
    return f"""
═══════════════════════════════════════════════════════════════════
GitHub Personal Access Token 生成指引
═══════════════════════════════════════════════════════════════════

推荐用 Fine-grained token (新版, 权限更细, 90 天有效, 推荐)

① 打开 (二选一):
   - Fine-grained (推荐): {SETUP_URLS['fine-grained']}
   - Classic (老版):      {SETUP_URLS['classic']}

② Fine-grained token 步骤:
   1. 点击 "Generate new token"
   2. Token name: ai_tools (或随便起名)
   3. Expiration: 90 days (默认, 过期前会提醒)
   4. Repository access: 选择
      ○ Public Repositories (public only) - 只能 push public repo
      ● All repositories - 推荐 (push 所有)
      ○ Only select repositories - 最小权限, 推荐: happylihappyli/*
   5. Permissions (只勾这些就够了):
      ● Contents: Read and write   ← 必勾 (clone/push 代码)
      ● Metadata: Read-only        ← 默认
      ○ Pull requests: Read and write (可选, 开 PR 用)
      ○ Issues: Read and write     (可选)
   6. 点击 "Generate token"
   7. ⚠️ 立即复制 token! (只显示一次, 关掉就再也看不到)

③ 保存到本机 (二选一):
   a) 命令行 (推荐, 自动):
        github-token --set ghp_xxxxxxxxxxxxxxxxxxxx
        或:
        echo "https://oauth2:ghp_xxxx@github.com" > ~/.git-credentials

   b) 环境变量 (临时, 关掉 terminal 失效):
        export GITHUB_TOKEN=ghp_xxxxxxxxxxxxxxxxxxxx

④ 验证:
        github-token --check
        github-token --diagnose

═══════════════════════════════════════════════════════════════════
如果 push 报 403 "Permission denied":
═══════════════════════════════════════════════════════════════════
  → Token 失效 (过期/被撤销/权限不够), 重新生成
  → 或 repo 是 private 但 token 没勾该 repo
  → 或 token 是 fine-grained 但 Resources 只勾了部分 repo

═══════════════════════════════════════════════════════════════════
"""


def set_token(token: str, dry: bool = False) -> int:
    """保存 token 到 ~/.git-credentials."""
    token = token.strip()
    if not token:
        sys.stderr.write("✗ token 为空\n")
        return 1
    cred_path = Path.home() / ".git-credentials"
    new_url = f"https://oauth2:{token}@github.com"
    # 先看现有内容
    existing = []
    if cred_path.exists():
        existing = cred_path.read_text(encoding="utf-8", errors="ignore").splitlines()
    # 删掉所有 github.com 行
    kept = []
    removed = 0
    for line in existing:
        if "github.com" in line and ("oauth2:" in line or "x-access-token" in line):
            removed += 1
            continue
        if line.strip():
            kept.append(line)
    kept.append(new_url)
    if dry:
        print(f"[dry-run] 将写 {cred_path}:")
        for ln in kept:
            print(f"  {ln}")
        if removed:
            print(f"  (清掉 {removed} 行旧 github.com 条目)")
        return 0
    # 写文件 (chmod 600 避免泄露)
    cred_path.write_text("\n".join(kept) + "\n", encoding="utf-8")
    try:
        os.chmod(cred_path, 0o600)
    except Exception:
        pass
    print(f"✓ 已保存 token 到 {cred_path}  (chmod 600)")
    print(f"  删了 {removed} 行旧 github.com 条目")
    # 自动验证
    print()
    print("验证新 token...")
    return do_check(verbose=True)


def clear_token(dry: bool = False) -> int:
    """清掉 ~/.git-credentials 里所有 github.com 条目."""
    cred_path = Path.home() / ".git-credentials"
    if not cred_path.exists():
        print(f"  {cred_path} 不存在, 无需清")
        return 0
    lines = cred_path.read_text(encoding="utf-8", errors="ignore").splitlines()
    kept = []
    removed = 0
    for line in lines:
        if "github.com" in line and ("oauth2:" in line or "x-access-token" in line):
            removed += 1
            continue
        if line.strip():
            kept.append(line)
    if dry:
        print(f"[dry-run] 将从 {cred_path} 删 {removed} 行 github.com 条目")
        return 0
    cred_path.write_text("\n".join(kept) + "\n" if kept else "", encoding="utf-8")
    print(f"✓ 删了 {removed} 行 github.com 条目")
    if not kept:
        cred_path.unlink()
        print(f"  (文件已删, 无其他条目)")
    return 0


def do_check(verbose: bool = True) -> int:
    """CLI: 检当前 token."""
    token, source = find_token()
    if not token:
        print("✗ 没找到任何 token")
        print("  设置方法: github-token --set <TOKEN>")
        print("  或:        export GITHUB_TOKEN=<TOKEN>")
        print()
        print(get_setup_instructions())
        return 1
    st = check_token(token)
    st.source = source
    if verbose:
        print_status(st)
    return 0 if st.valid else 1


def do_diagnose(verbose: bool = True) -> int:
    """CLI: 完整诊断."""
    st = diagnose(verbose=False)
    if verbose:
        print_status(st)
    return 0 if st.valid else 1


def main() -> int:
    p = argparse.ArgumentParser(
        prog="github-token",
        description="GitHub Personal Access Token 管理工具",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="例: github-token --check | --diagnose | --set <TOKEN> | --setup",
    )
    p.add_argument("--check", action="store_true", help="检当前 token 是否有效")
    p.add_argument("--diagnose", action="store_true", help="完整诊断 (含所有 repo 访问权限)")
    p.add_argument("--set", metavar="TOKEN", help="保存新 token 到 ~/.git-credentials")
    p.add_argument("--clear", action="store_true", help="清掉 ~/.git-credentials 里的 github.com 条目")
    p.add_argument("--setup", action="store_true", help="打印生成新 token 的 step-by-step 指引")
    p.add_argument("--url", action="store_true", help="只打印生成 token 的 URL (不显示指引)")
    p.add_argument("--info", action="store_true", help="显示 token 来源 / 状态")
    p.add_argument("--dry", action="store_true", help="配合 --set/--clear, 只显示不真做")
    p.add_argument("-q", "--quiet", action="store_true", help="静默模式")
    args = p.parse_args()

    if args.url:
        print(f"Fine-grained (推荐): {SETUP_URLS['fine-grained']}")
        print(f"Classic (老版):      {SETUP_URLS['classic']}")
        return 0
    if args.setup:
        print(get_setup_instructions())
        return 0
    if args.set:
        return set_token(args.set, dry=args.dry)
    if args.clear:
        return clear_token(dry=args.dry)
    if args.diagnose:
        return do_diagnose(verbose=not args.quiet)
    if args.check or args.info:
        return do_check(verbose=not args.quiet)

    # 默认: 等价于 --check
    p.print_help()
    return 1


if __name__ == "__main__":
    sys.exit(main())
