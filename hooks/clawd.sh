#!/bin/bash
# Claude Code hook：把状态发给桌上的 Clawd。
# 用法：clawd.sh <idle|thinking|working|waiting|done|sleep|notify>，hook 的 JSON 从 stdin 传入。
# notify 给 Notification 用，按 notification_type 决定发什么。
# 设备地址默认 clawd.local，可用环境变量 CLAWD_HOST 改成 IP。

host="${CLAWD_HOST:-clawd.local}"
state="$1"
input=$(cat)

# 一次取出会话 id、项目目录、工具名等。用 \x1f 分隔：tab 是空白字符，空字段会被 read 合并掉
# detail：AskUserQuestion 取第一个问题的标题，Bash 取命令，改文件的取文件名
IFS=$'\x1f' read -r sid cwd tool ntype detail < <(
  jq -r '[.session_id // "", .cwd // "", .tool_name // "", .notification_type // "",
          ((.tool_input // {}) as $i
           | ($i.questions[0]? | (.header // .question)?) // $i.command // ($i.file_path? | strings | split("/") | last) // ""
           | tostring | gsub("[\\n\\r\u001f]"; " ") | .[0:60])
         ] | join("\u001f")' <<<"$input" 2>/dev/null
)
sid="${sid:0:8}"      # 每个会话一只螃蟹，前 8 位足够区分
name="${cwd##*/}"     # 项目目录名，多只螃蟹同屏时显示在头顶
tool="${tool##*__}"   # mcp__server__tool -> tool

if [ "$state" = notify ]; then
  case "$ntype" in
    permission_prompt|elicitation_dialog|elicitation_url_dialog|agent_needs_input) state=waiting ;;
    elicitation_complete|elicitation_response) state=thinking ;;
    idle_prompt) state=idle ;;   # 停下约 60 秒没打字。按 Esc 打断不触发 Stop，靠这个兜底
    *) exit 0 ;;
  esac
fi

# 会停下来等人回答的工具：bypass 模式下也会问，按 waiting 算
if [ "$state" = working ]; then
  case "$tool" in
    AskUserQuestion) state=waiting ;;
    ExitPlanMode) state=waiting detail="plan ready" ;;
  esac
fi

label=""
case "$state" in
  working)
    label="$tool"
    ;;
  waiting)
    case "$tool" in
      AskUserQuestion|ExitPlanMode|Bash) label="${detail:-$tool}" ;;   # 屏幕窄，Bash 只显示命令
      "") label="" ;;
      *) label="$tool${detail:+: $detail}" ;;
    esac
    ;;
esac

# 毫秒时间戳，设备用它丢弃乱序到达的旧状态
stamp=$(perl -MTime::HiRes=time -e 'printf "%d", time*1000')

curl -4 -s -G -m 2 --connect-timeout 1 "http://$host/state" \
  --data-urlencode "s=$state" \
  --data-urlencode "sid=$sid" \
  --data-urlencode "name=$name" \
  --data-urlencode "label=$label" \
  --data-urlencode "t=$stamp" >/dev/null 2>&1

exit 0
