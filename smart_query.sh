#!/bin/bash
# 查询工具 - 人性化时间输入和显示

set -e

# 颜色输出
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# 获取脚本的绝对路径
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# 默认配置
DEFAULT_DB_DIR="$SCRIPT_DIR/tmp"
DEFAULT_LOG_DIR="$SCRIPT_DIR/log"
QUERY_TOOL="./query_tool"

# 确保日志目录存在，且在不可写时回退到 /tmp，避免普通用户权限报错
mkdir -p "$DEFAULT_LOG_DIR" >/dev/null 2>&1 || true
if [[ -w "$DEFAULT_LOG_DIR" ]]; then
  LOG_FILE="$DEFAULT_LOG_DIR/query_tool_debug_$(id -u).log"
else
  LOG_FILE="/tmp/query_tool_debug_$(id -u).log"
fi

# 显示帮助信息
show_help() {
    cat << EOF
${GREEN}查询工具 - 时间查询${NC}

用法: $0 [选项]

选项:
  --dir <目录>        数据库目录 (默认: $DEFAULT_DB_DIR)
  --time <时间段>     人性化时间格式
  --pid <pid>         按进程ID过滤
  --name <名称>       按进程名过滤
  --help              显示此帮助
  --flame             输出火焰图兼容格式
  --detailed          输出详细调用栈信息
  --stats-file <文件> 对指定的 folded 栈文件输出统计信息（不依赖外部引号/转义）

时间格式示例:
  --time "15:10-15:30"          # 今天的15:10-15:30
  --time "2023-09-22 15:10-15:30"  # 指定日期
  --time "today 15:10"          # 今天15:10到现在
  --time "last 20 minutes"      # 最近20分钟
  --time "09:00-"               # 09:00到现在
  --time "-15:30"               # 今天开始到15:30

示例:
  $0 --time "15:10-15:30"
  $0 --time "today 14:00" --pid 1234
  $0 --time "last 30 minutes" --name nginx
  $0 --time "last 5 minutes" --flame > flame_data.txt
  $0 --time "last 5 minutes" --detailed --pid 1234
  $0 --stats-file flame_folded.txt
EOF
}

# 解析人性化时间
parse_time_range() {
    local time_str="$1"
    local now=$(date +%s)

    case "$time_str" in
        "last"*"minutes"*)
            local minutes=${time_str#*last }
            minutes=${minutes% minutes*}
            START_TIME=$((now - minutes * 60))
            END_TIME=$now
            ;;
        "last"*"years"*)
            local years=${time_str#*last }
            years=${years% years*}
            START_TIME=$((now - years * 365 * 24 * 60 * 60))
            END_TIME=$now
            ;;
        "today"*)
            local today=$(date +%Y-%m-%d)
            local time_part=${time_str#today }
            if [[ "$time_part" == *"-"* ]]; then
                local start_time=${time_part%-*}
                local end_time=${time_part#*-}
                START_TIME=$(date -d "$today $start_time" +%s 2>/dev/null || echo $now)
                END_TIME=$(date -d "$today $end_time" +%s 2>/dev/null || echo $now)
            else
                START_TIME=$(date -d "$today $time_part" +%s 2>/dev/null || echo $now)
                END_TIME=$now
            fi
            ;;
        *"-"*)
            local start_time=${time_str%-*}
            local end_time=${time_str#*-}
            if [[ "$start_time" == *"-"*"-"* ]]; then
                # 完整日期格式
                START_TIME=$(date -d "$start_time" +%s 2>/dev/null || echo $now)
                END_TIME=$(date -d "$end_time" +%s 2>/dev/null || echo $now)
            else
                # 今天的相对时间
                local today=$(date +%Y-%m-%d)
                START_TIME=$(date -d "$today $start_time" +%s 2>/dev/null || echo $now)
                END_TIME=$(date -d "$today $end_time" +%s 2>/dev/null || echo $now)
            fi
            ;;
        *)
            echo -e "${RED}错误: 无法解析时间格式 '$time_str'${NC}"
            exit 1
            ;;
    esac
}

# 人性化时间显示
format_output() {
    while IFS='|' read -r stack count; do
        if [[ -n "$stack" ]]; then
            # 火焰图格式：进程名;函数1;函数2;函数3
            echo "$stack $count"
        fi
    done
}

# 火焰图格式输出
format_flame() {
    awk '
    function demangle(name) {
        if (name ~ /^_Z/) {
            cmd = "c++filt -n " name
            cmd | getline dem
            close(cmd)
            if (dem != "") return dem
        }
        return name
    }
    {
        if (!match($0, /^(.*) ([0-9]+)$/, arr)) { print $0; next }
        stack = arr[1]
        count = arr[2] + 0

        n = split(stack, parts, ";")
        if (n < 1) next

        process = parts[1]
        pid = ""
        if (match(process, /\[([0-9]+)\]$/, pm)) pid = pm[1]

        # collect frames
        user_frames_count = 0
        kernel_frames_count = 0
        for (i = 2; i <= n; i++) {
            f = parts[i]
            # normalize unknown_process with pid to avoid cross-process aggregation
            if (f == "[unknown_process]" || f == "unknown_process") {
                if (pid != "") f = "unknown_process[" pid "]"
                else f = "unknown_process"
            }
            f = demangle(f)
            if (f ~ /\[kernel\]$/) kernel_frames[++kernel_frames_count] = f
            else user_frames[++user_frames_count] = f
        }

        new_stack = process

        # 需求：自下而上顺序为 用户入口 -> 用户调用 -> 内核入口 -> 内核调用
        # 说明：折叠栈原始顺序多为“调用在前、入口在后”（接近叶子在前，根在后）。
        # 因此需要分别对用户帧与内核帧逆序，以保证先输出入口（靠近根）再输出调用（靠近叶子）。
        if (user_frames_count == 0) {
            new_stack = new_stack ";" "no_user_stack"
        } else {
            # 先输出用户入口（逆序，从靠近根到靠近叶子）
            for (u = user_frames_count; u >= 1; u--) new_stack = new_stack ";" user_frames[u]
        }
        if (kernel_frames_count > 0) {
            # 再输出内核入口与调用（同样逆序）
            for (k = kernel_frames_count; k >= 1; k--) new_stack = new_stack ";" kernel_frames[k]
        }

        print new_stack " " count
    }
    '
}

# 详细显示格式
format_detailed() {
    while IFS='|' read -r stack count; do
        if [[ -n "$stack" ]]; then
            # 解析火焰图格式
            IFS=';' read -ra funcs <<< "$stack"
            if [[ ${#funcs[@]} -gt 0 ]]; then
                process_name="${funcs[0]}"
                echo "[$process_name] $count次"
                for ((i=1; i<${#funcs[@]}; i++)); do
                    printf "  %s\n" "${funcs[i]}"
                done
                echo "---"
            else
                echo "$stack $count"
            fi
        fi
    done
}

# 统计 folded 栈文件
compute_stats() {
    local file="$1"
    if [[ ! -f "$file" ]]; then
        echo -e "${RED}错误: 文件 '$file' 不存在${NC}" >&2
        exit 1
    fi

    # 总计
    awk '/;/ && $NF ~ /^[0-9]+$/ {c=$NF; n+=1; t+=c} END {printf("STAT: Total valid lines: %d\nSTAT: Total samples: %d\n", n+0, t+0)}' "$file"

    echo 'STAT: First 3 lines:'
    head -n 3 "$file" | sed 's/^/STAT: /'

    echo 'STAT: Last 3 lines:'
    tail -n 3 "$file" | sed 's/^/STAT: /'

    # Top 10 进程
    echo 'STAT: Top 10 processes:'
    awk '/;/ && $NF ~ /^[0-9]+$/ {c=$NF; sub(/[[:space:]][0-9]+$/, "", $0); split($0,a,";"); p=a[1]; print c "\t" p}' "$file" \
      | awk -F '\t' '{s[$2]+=$1} END{for(k in s) printf "%d\t%s\n", s[k], k}' \
      | sort -nr | head -10 | sed 's/^/STAT: /'

    # Top 10 叶子帧
    echo 'STAT: Top 10 leaf frames:'
    awk '/;/ && $NF ~ /^[0-9]+$/ {c=$NF; sub(/[[:space:]][0-9]+$/, "", $0); split($0,a,";"); lf=a[length(a)]; print c "\t" lf}' "$file" \
      | awk -F '\t' '{s[$2]+=$1} END{for(k in s) printf "%d\t%s\n", s[k], k}' \
      | sort -nr | head -10 | sed 's/^/STAT: /'

    # Top 10 总帧
    echo 'STAT: Top 10 overall frames:'
    awk '/;/ && $NF ~ /^[0-9]+$/ {c=$NF; sub(/[[:space:]][0-9]+$/, "", $0); split($0,a,";"); for(i=1;i<=length(a);i++){print c "\t" a[i]}}' "$file" \
      | awk -F '\t' '{s[$2]+=$1} END{for(k in s) printf "%d\t%s\n", s[k], k}' \
      | sort -nr | head -10 | sed 's/^/STAT: /'

    # Unknown 占比（按样本加权）
    echo 'STAT: Unknown frames ratio (by samples):'
    awk '/;/ && $NF ~ /^[0-9]+$/ {c=$NF; sub(/[[:space:]][0-9]+$/, "", $0); stack=$0; t+=c; if (stack ~ /(^|;)\[(unknown|unknown_process)\](;|$)/ || stack ~ /(^|;).*unknown.*(;|$)/) u+=c} END {printf("STAT: Unknown samples: %d\nSTAT: Unknown %%: %.2f%%\n", u+0, (t? (u*100.0/t):0))}' "$file"
}

# 主函数
main() {
    local db_dir="$DEFAULT_DB_DIR"
    local time_range=""
    local pid_filter=""
    local name_filter=""
    local stats_file=""

    # 解析参数
    while [[ $# -gt 0 ]]; do
        case $1 in
            --dir)
                db_dir="$2"
                shift 2
                ;;
            --time)
                time_range="$2"
                shift 2
                ;;
            --pid)
                pid_filter="$2"
                shift 2
                ;;
            --name)
                name_filter="$2"
                shift 2
                ;;
            --help)
                show_help
                exit 0
                ;;
            --flame)
                format_func="format_flame"
                shift
                ;;
            --detailed)
                format_func="format_detailed"
                shift
                ;;
            --stats-file)
                stats_file="$2"
                shift 2
                ;;
            *)
                echo -e "${RED}错误: 未知参数 '$1'${NC}"
                show_help
                exit 1
                ;;
        esac
    done

    # 如果提供了统计目标文件，则直接输出统计并退出
    if [[ -n "$stats_file" ]]; then
        compute_stats "$stats_file"
        exit 0
    fi

    # 检查必需参数
    if [[ -z "$time_range" ]]; then
        echo -e "${RED}错误: 必须指定 --time 参数${NC}"
        show_help
        exit 1
    fi

    # 检查数据库目录
    if [[ ! -d "$db_dir" ]]; then
        echo -e "${RED}错误: 数据库目录 '$db_dir' 不存在${NC}"
        exit 1
    fi

    # 检查query_tool
    if [[ ! -f "$QUERY_TOOL" ]]; then
        echo -e "${RED}错误: query_tool 未找到${NC}"
        exit 1
    fi

    # 解析时间范围
    parse_time_range "$time_range"

    echo -e "${GREEN}查询时间范围: $(date -d "@$START_TIME" '+%Y-%m-%d %H:%M:%S') 到 $(date -d "@$END_TIME" '+%Y-%m-%d %H:%M:%S')${NC}" >&2
    echo -e "${YELLOW}DEBUG: START_TIME=$START_TIME, END_TIME=$END_TIME${NC}" >&2

    # 构建查询参数
    local query_args=()
    [[ -n "$pid_filter" ]] && query_args+=(--pid "$pid_filter")
    [[ -n "$name_filter" ]] && query_args+=(--name "$name_filter")

    # 根据时间格式判断是实时查询还是历史查询，并直接执行
    local result
    if [[ "$time_range" == "last"* ]]; then
        echo -e "${GREEN}执行实时查询 (HTTP模式)...${NC}" >&2
        # 在命令前直接设置环境变量，仅对该命令生效。使用数组 ("${args[@]}") 安全地传递可选参数。
        result=$(no_proxy=localhost,127.0.0.1 "$QUERY_TOOL" --start-time "$START_TIME" --end-time "$END_TIME" "${query_args[@]}" 2>>"$LOG_FILE" || true)
    else
        echo -e "${GREEN}执行历史查询 (SQLite模式)...${NC}" >&2
        # 检查数据库目录
        if [[ ! -d "$db_dir" ]]; then
            echo -e "${RED}错误: 数据库目录 '$db_dir' 不存在${NC}"
            exit 1
        fi
        result=$("$QUERY_TOOL" --dir "$db_dir" --start-time "$START_TIME" --end-time "$END_TIME" "${query_args[@]}" 2>>"$LOG_FILE" || true)
    fi

    if [[ -z "$result" ]]; then
        echo -e "${YELLOW}在指定的时间范围内没有找到任何数据。${NC}" >&2
    else
        echo "$result" | sed 's/|/ /g' | ${format_func:-format_output}
    fi
}

# 执行主函数
main "$@"