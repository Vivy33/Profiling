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
            local end_time=${time_part#*-}
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
            echo "$stack ($count)"
        fi
    done
}

# 主函数
main() {
    local db_dir="$DEFAULT_DB_DIR"
    local time_range=""
    local pid_filter=""
    local name_filter=""

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
            *)
                echo -e "${RED}错误: 未知参数 '$1'${NC}"
                show_help
                exit 1
                ;;
        esac
    done

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

    echo -e "${GREEN}查询时间范围: $(date -d "@$START_TIME" '+%Y-%m-%d %H:%M:%S') 到 $(date -d "@$END_TIME" '+%Y-%m-%d %H:%M:%S')${NC}"
    echo -e "${YELLOW}DEBUG: START_TIME=$START_TIME, END_TIME=$END_TIME${NC}"

    # 构建查询命令
    local query_cmd="$QUERY_TOOL --dir $db_dir --start-time $START_TIME --end-time $END_TIME"
    [[ -n "$pid_filter" ]] && query_cmd="$query_cmd --pid $pid_filter"
    [[ -n "$name_filter" ]] && query_cmd="$query_cmd --name $name_filter"

    # 执行查询并格式化输出
    $query_cmd 2>"$DEFAULT_LOG_DIR/query_tool_debug.log" | format_output
}

# 执行主函数
main "$@"