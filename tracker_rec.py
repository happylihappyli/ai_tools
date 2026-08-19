# -*- coding: utf-8 -*-
"""
tracker_rec.py — 步骤记录 helper
================================
对 step_data.json 的直接读写, 不依赖 step_tracker.py 的 GUI 部分。
任何 Python 环境都能用。

API:
  set_goal(goal)                 — 设主目标
  add_node(id, title, ...)       — 加节点 (id 重复自动跳过)
  update_node(id, ...)           — 改节点 (status/desc/title/next/x/y)
  set_current(id)                — 设当前节点 (自动 in_progress)
  start_action(id, desc=...)     — 设当前 + in_progress + 写描述
  complete_action(id, desc=...)  — completed + 写描述
  fail_action(id, desc=...)      — pending + 写错误描述
  list_nodes()                   — 返回所有节点
  get_node(id)                   — 按 id 取节点

  事件日志 (events[]) — 给 compile_tool / 其他工具记每次操作:
  add_event(type, title, desc="", node_id=None) — 追加一条事件, 返回事件 id
  list_events(limit=100)         — 返回最近 N 条事件 (新→旧)
  clear_events()                 — 清空事件日志
"""

import json
import os
import time
from pathlib import Path
from typing import Optional, List, Dict, Any
from datetime import datetime


# ===== 数据文件路径解析 (env var + 默认) =====
DEFAULT_DATA_FILE = Path("/home/bv/code/ai_tools/step_data.json")


def _resolve_data_file() -> Path:
    """解析数据文件路径。
    优先级: 1) os.environ['TRACKER_DATA_FILE']  2) 默认 DEFAULT_DATA_FILE
    不会抛错, 路径不存在也能返回。
    """
    env = os.environ.get("TRACKER_DATA_FILE")
    if env:
        return Path(env).expanduser().resolve()
    return DEFAULT_DATA_FILE


# 模块加载时解析一次, 之后 set_data_file() 可改
DATA_FILE = _resolve_data_file()


def set_data_file(path) -> None:
    """运行时切换数据文件路径 (CLI 传 --data-file 时调用)。"""
    global DATA_FILE
    DATA_FILE = Path(path).expanduser().resolve()


def get_data_file() -> Path:
    """返回当前数据文件路径。"""
    return DATA_FILE


def _load() -> Dict:
    """读 step_data.json, 不存在或损坏返回默认结构。"""
    if not DATA_FILE.exists():
        return {"main_goal": "", "current_node": None, "nodes": [], "events": []}
    try:
        with open(DATA_FILE, "r", encoding="utf-8") as f:
            data = json.load(f)
        # 兼容老数据: 没 events 字段就补
        if "events" not in data:
            data["events"] = []
        return data
    except (json.JSONDecodeError, OSError):
        return {"main_goal": "", "current_node": None, "nodes": [], "events": []}


def _save(data: Dict) -> None:
    """写 step_data.json (原子写: 写临时文件 + rename, 防崩溃丢数据)."""
    tmp = DATA_FILE.with_suffix(".json.tmp")
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(data, f, ensure_ascii=False, indent=2)
    tmp.replace(DATA_FILE)


def list_nodes() -> List[Dict]:
    """返回所有节点。"""
    return list(_load().get("nodes", []))


def get_node(node_id: str) -> Optional[Dict]:
    """按 id 查节点, 找不到返回 None。"""
    for n in list_nodes():
        if n["id"] == node_id:
            return n
    return None


def set_goal(goal: str) -> bool:
    """设主目标。"""
    data = _load()
    data["main_goal"] = goal
    _save(data)
    return True


def add_node(node_id: str, title: str, desc: str = "",
             status: str = "pending", x: float = 0, y: float = 0,
             next_nodes: Optional[List[str]] = None) -> bool:
    """加节点 (id 重复自动跳过, 不报错)."""
    data = _load()
    if any(n["id"] == node_id for n in data["nodes"]):
        return False  # 已存在, 跳过
    data["nodes"].append({
        "id": str(node_id),
        "title": title,
        "description": desc,
        "status": status,
        "x": x,
        "y": y,
        "next": list(next_nodes) if next_nodes else [],
    })
    _save(data)
    return True


def update_node(node_id: str, status: Optional[str] = None,
                desc: Optional[str] = None,
                title: Optional[str] = None,
                next_nodes: Optional[List[str]] = None,
                x: Optional[float] = None,
                y: Optional[float] = None) -> bool:
    """改节点字段; 传 None 的字段保持不变。传 desc="" 也可以清空。"""
    data = _load()
    for n in data["nodes"]:
        if n["id"] == str(node_id):
            if status is not None:
                n["status"] = status
            if desc is not None:
                n["description"] = desc
            if title is not None:
                n["title"] = title
            if next_nodes is not None:
                n["next"] = list(next_nodes)
            if x is not None:
                n["x"] = x
            if y is not None:
                n["y"] = y
            _save(data)
            return True
    return False  # 不存在


def set_current(node_id: str) -> bool:
    """设当前节点 (自动 in_progress)."""
    data = _load()
    if not any(n["id"] == str(node_id) for n in data["nodes"]):
        return False
    data["current_node"] = str(node_id)
    for n in data["nodes"]:
        if n["id"] == str(node_id):
            n["status"] = "in_progress"
            break
    _save(data)
    return True


def start_action(node_id: str, desc: str = "") -> bool:
    """开始一个动作: 设当前 + 状态 in_progress + (可选)更新描述。"""
    update_node(node_id, status="in_progress", desc=desc)
    return set_current(node_id)


def complete_action(node_id: str, desc: str = "") -> bool:
    """完成一个动作: 状态 completed + (可选)更新描述。"""
    return update_node(node_id, status="completed", desc=desc)


def fail_action(node_id: str, desc: str = "") -> bool:
    """失败: 状态回到 pending + 记录错误描述。"""
    return update_node(node_id, status="pending", desc=desc)


def remove_node(node_id: str) -> bool:
    """删节点 (小心使用, 不清理其他节点的 next 引用)。"""
    data = _load()
    before = len(data["nodes"])
    data["nodes"] = [n for n in data["nodes"] if n["id"] != str(node_id)]
    if data.get("current_node") == str(node_id):
        data["current_node"] = None
    if len(data["nodes"]) < before:
        _save(data)
        return True
    return False


# ===== 事件日志 (events[]) =====
def _now_ts() -> str:
    """返回 ISO-8601 时间戳 (秒精度, 本地时区)。"""
    return datetime.now().isoformat(timespec="seconds")


def add_event(event_type: str, title: str, desc: str = "",
              node_id: Optional[str] = None) -> int:
    """追加一条事件到 events[] 末尾。返回新事件的 id (自增)。
    同类型的事件太多会保留最近 200 条, 自动丢弃更早的。
    event_type:  "compile_start" / "compile_finish" / "compile_fail" / "clean" /
                 "deep_clean" / "touch" / "diagnose" / "launch" / "info" / "error" /
                 或其他自定义字符串
    """
    data = _load()
    events = data.setdefault("events", [])
    next_id = (max((e["id"] for e in events), default=0)) + 1
    ev = {
        "id": next_id,
        "ts": _now_ts(),
        "type": str(event_type),
        "title": str(title),
        "desc": str(desc),
        "node_id": str(node_id) if node_id else None,
    }
    events.append(ev)
    # 保留最近 200 条
    if len(events) > 200:
        data["events"] = events[-200:]
    _save(data)
    return next_id


def list_events(limit: int = 100, event_type: Optional[str] = None,
                node_id: Optional[str] = None) -> List[Dict]:
    """返回事件列表 (新→旧, 最多 limit 条)。
    event_type: 可选, 只返回此类型的事件
    node_id:    可选, 只返回与该节点关联的事件
    """
    events = _load().get("events", [])
    if event_type:
        events = [e for e in events if e.get("type") == event_type]
    if node_id:
        events = [e for e in events if e.get("node_id") == node_id]
    # 新→旧
    return list(reversed(events[-limit:]))


def clear_events() -> bool:
    """清空所有事件。"""
    data = _load()
    data["events"] = []
    _save(data)
    return True


def latest_event(event_type: Optional[str] = None) -> Optional[Dict]:
    """返回最新一条事件 (新→旧, 所以 reversed 的第一个)。可选过滤类型。"""
    evs = list_events(limit=1, event_type=event_type)
    return evs[0] if evs else None


# ===== CLI =====
def _cli_main(args: List[str]) -> int:
    """简单 CLI:  list | show ID | goal "..." | add ... | update ... | current ID | remove ID"""
    import argparse
    p = argparse.ArgumentParser(prog="tracker_rec.py", description="直接读写 step_data.json")
    p.add_argument("--data-file", default=None,
                   help=f"step_data.json 路径 (默认 {DEFAULT_DATA_FILE}, "
                        f"也可用 env TRACKER_DATA_FILE)")
    sub = p.add_subparsers(dest="cmd", required=True)

    sub.add_parser("list", help="列出所有节点")

    p_show = sub.add_parser("show", help="看节点详情")
    p_show.add_argument("id")

    p_goal = sub.add_parser("goal", help="设主目标")
    p_goal.add_argument("text")

    p_add = sub.add_parser("add", help="加节点")
    p_add.add_argument("--id", required=True)
    p_add.add_argument("--title", required=True)
    p_add.add_argument("--desc", default="")
    p_add.add_argument("--status", default="pending")
    p_add.add_argument("--next", default="")

    p_up = sub.add_parser("update", help="改节点")
    p_up.add_argument("--id", required=True)
    p_up.add_argument("--status", default=None)
    p_up.add_argument("--desc", default=None)
    p_up.add_argument("--title", default=None)
    p_up.add_argument("--next", default=None)

    p_cur = sub.add_parser("current", help="设当前")
    p_cur.add_argument("id")

    p_rm = sub.add_parser("remove", help="删节点")
    p_rm.add_argument("id")

    # ===== 事件日志子命令 =====
    p_ev_list = sub.add_parser("events", help="列最近事件 (新→旧)")
    p_ev_list.add_argument("--limit", type=int, default=50)
    p_ev_list.add_argument("--type", default=None, help="只显示该类型")
    p_ev_list.add_argument("--node", default=None, help="只显示该节点")

    p_ev_add = sub.add_parser("event", help="追加一条事件")
    p_ev_add.add_argument("type", help="事件类型, 如 compile_start")
    p_ev_add.add_argument("title", help="事件标题")
    p_ev_add.add_argument("--desc", default="", help="详细描述")
    p_ev_add.add_argument("--node", default=None, help="关联节点 id")

    p_ev_clear = sub.add_parser("clear-events", help="清空事件日志")

    args = p.parse_args(args)

    # 全局: --data-file 覆盖
    if getattr(args, "data_file", None):
        set_data_file(args.data_file)
        print(f"📁 数据文件: {DATA_FILE}", file=__import__("sys").stderr)

    if args.cmd == "list":
        d = _load()
        print(f"主目标: {d.get('main_goal') or '(未设置)'}")
        print(f"当前节点: #{d.get('current_node')}" if d.get("current_node") else "当前节点: (无)")
        print(f"共 {len(d['nodes'])} 个节点:")
        for n in d["nodes"]:
            mark = " ▶" if n["id"] == d.get("current_node") else ""
            print(f"  #{n['id']} [{n['status']}] {n['title']}{mark}")
        return 0
    if args.cmd == "show":
        n = get_node(args.id)
        if not n:
            print(f"✗ 节点 #{args.id} 不存在", file=__import__("sys").stderr)
            return 1
        print(f"#{n['id']}  {n['title']}")
        print(f"  状态: {n['status']}")
        print(f"  描述: {n.get('description', '(无)')}")
        print(f"  后续: {', '.join(n.get('next', [])) or '(无)'}")
        return 0
    if args.cmd == "goal":
        set_goal(args.text)
        print(f"✓ 主目标: {args.text}")
        return 0
    if args.cmd == "add":
        nxt = [s.strip() for s in (args.next or "").split(",") if s.strip()] or None
        if add_node(args.id, args.title, args.desc, args.status, next_nodes=nxt):
            print(f"✓ 已添加 #{args.id}: {args.title}")
        else:
            print(f"(#{args.id} 已存在, 跳过)")
        return 0
    if args.cmd == "update":
        nxt = [s.strip() for s in args.next.split(",") if s.strip()] if args.next else None
        if update_node(args.id, status=args.status, desc=args.desc, title=args.title, next_nodes=nxt):
            print(f"✓ #{args.id} 已更新")
            return 0
        print(f"✗ #{args.id} 不存在", file=__import__("sys").stderr)
        return 1
    if args.cmd == "current":
        if set_current(args.id):
            print(f"▶ 当前节点 → #{args.id}")
            return 0
        print(f"✗ #{args.id} 不存在", file=__import__("sys").stderr)
        return 1
    if args.cmd == "remove":
        if remove_node(args.id):
            print(f"🗑 已删除 #{args.id}")
            return 0
        print(f"✗ #{args.id} 不存在", file=__import__("sys").stderr)
        return 1
    if args.cmd == "events":
        evs = list_events(limit=args.limit, event_type=args.type, node_id=args.node)
        print(f"共 {len(evs)} 条事件 (新→旧):")
        for e in evs:
            node = f" [#{e['node_id']}]" if e.get("node_id") else ""
            desc = f"  — {e['desc']}" if e.get("desc") else ""
            print(f"  #{e['id']:>4} {e['ts']}  [{e['type']}]{node}  {e['title']}{desc}")
        return 0
    if args.cmd == "event":
        eid = add_event(args.type, args.title, args.desc, args.node)
        print(f"✓ 已记录事件 #{eid}: [{args.type}] {args.title}")
        return 0
    if args.cmd == "clear-events":
        clear_events()
        print("🧹 事件日志已清空")
        return 0
    return 0


if __name__ == "__main__":
    import sys
    if len(sys.argv) <= 1:
        # 自测
        print("=== tracker_rec self-test ===")
        nodes_before = len(list_nodes())
        print(f"现有节点: {nodes_before}")
        add_node("tracker_test", "tracker_rec 自测", "自动测试", "in_progress", x=999, y=999)
        set_current("tracker_test")
        print("current: tracker_test")
        update_node("tracker_test", status="completed", desc="自测完成")
        n = get_node("tracker_test")
        print(f"verify: status={n['status']}, desc={n['description']}, x={n['x']}")
        remove_node("tracker_test")
        print(f"removed. nodes after: {len(list_nodes())}")
        print("=== OK ===")
    else:
        sys.exit(_cli_main(sys.argv[1:]))
