#!/bin/bash
# 性能分析自动化管理脚本

set -e

# 颜色输出
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

# 配置
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SERVICE_NAME="profiling-cleanup"
TIMER_NAME="profiling-cleanup"
DEFAULT_DB_DIR="$SCRIPT_DIR/tmp"
DEFAULT_LOG_DIR="$SCRIPT_DIR/log"
DEFAULT_FREQUENCY=30
DAYS_TO_KEEP=7
DEFAULT_HTTP_PORT=8081
DEFAULT_DB_BATCH_SIZE=40
PROFILING_TOOL="$SCRIPT_DIR/profiling_tool"
QUERY_TOOL="$SCRIPT_DIR/query_tool"

# 显示帮助
show_help() {
    cat << EOF
${GREEN}性能分析统一工具${NC}

用法: $0 [命令] [选项]

${YELLOW}核心命令:${NC}
  start      启动性能分析（带自动清理）
  stop       停止性能分析
  status     查看系统状态
  cleanup    手动清理旧数据
  install    安装systemd定时任务
  uninstall  卸载systemd定时任务
  logs       查看日志

${YELLOW}start命令选项:${NC}
  --dir PATH          数据目录 (默认: $DEFAULT_DB_DIR)
  --frequency HZ      采样频率 (默认: $DEFAULT_FREQUENCY)
  --filter TYPE       过滤类型: user|kernel|all (默认: all)
  --cleanup SEC       清理间隔秒数 (默认: 30)
  --stack-depth NUM   最大栈深度 (默认: 48)
  --db-batch-size NUM 数据库写入批处理大小 (默认: $DEFAULT_DB_BATCH_SIZE)
  --lbr               启用LBR精确调用栈

${YELLOW}cleanup命令选项:${NC}
  --days NUM         保留天数 (默认: $DAYS_TO_KEEP)
  --dry-run          只显示不删除

${YELLOW}示例:${NC}
  $0 start                                    # 默认启动
  $0 start --frequency 100 --dir /data/prof   # 高频采样
  $0 cleanup --days 3 --dry-run               # 测试清理
  $0 status                                   # 查看状态
EOF
}

# 检查必需工具
check_requirements() {
    local missing=()

    [[ ! -f "$PROFILING_TOOL" ]] && missing+=("profiling_tool")

    if [[ ${#missing[@]} -gt 0 ]]; then
        echo -e "${RED}错误: 缺少必需工具: ${missing[*]}${NC}"
        exit 1
    fi
}

# 启动性能分析
start_profiling() {
    local db_dir="$DEFAULT_DB_DIR"
    local frequency="$DEFAULT_FREQUENCY"
    local filter="all"
    local cleanup_secs="30"
    local stack_depth="48"
    local db_batch_size="$DEFAULT_DB_BATCH_SIZE"
    local lbr=""

    # 解析参数
    while [[ $# -gt 0 ]]; do
        case $1 in
            --dir)
                db_dir="$2"
                shift 2
                ;;
            --frequency)
                frequency="$2"
                shift 2
                ;;
            --filter)
                filter="$2"
                shift 2
                ;;
            --cleanup)
                cleanup_secs="$2"
                shift 2
                ;;
            --stack-depth)
                stack_depth="$2"
                shift 2
                ;;
            --db-batch-size)
                db_batch_size="$2"
                shift 2
                ;;
            --lbr)
                lbr="--lbr"
                shift
                ;;
            *)
                echo -e "${RED}错误: 未知参数 '$1'${NC}"
                exit 1
                ;;
        esac
    done

    check_requirements

    # 检查是否已运行
    if pgrep -f "profiling_tool" > /dev/null; then
        echo -e "${YELLOW}警告: 性能分析已在运行${NC}"
        return 1
    fi

    # 检查是否有sudo权限
    if [[ $EUID -ne 0 ]]; then
        echo -e "${RED}错误: 性能分析需要root权限${NC}"
        echo -e "${YELLOW}请使用: sudo $0 start${NC}"
        return 1
    fi

    # 创建目录
    mkdir -p "$DEFAULT_DB_DIR" "$DEFAULT_LOG_DIR"

    # 构建命令
    local cmd="$PROFILING_TOOL --output-dir=$db_dir --frequency=$frequency --filter=$filter --cleanup=$cleanup_secs --stack-depth=$stack_depth --db-batch-size=$db_batch_size $lbr --http-port=$DEFAULT_HTTP_PORT --log-dir=$DEFAULT_LOG_DIR"

    printf "${GREEN}启动配置:${NC}\n"
    printf "  %-12s: %s\n" "日志目录" "${DEFAULT_LOG_DIR}"
    printf "  %-12s: %s\n" "数据目录" "${db_dir}"
    printf "  %-12s: %s Hz\n" "采样频率" "${frequency}"
    printf "  %-12s: %s\n" "过滤模式" "${filter}"
    printf "  %-12s: %s 秒\n" "清理间隔" "${cleanup_secs}"
    printf "  %-12s: %s 层\n" "最大栈深" "${stack_depth}"
    printf "  %-12s: %s\n" "批处理大小" "${db_batch_size}"
    [[ -n "$lbr" ]] && printf "  %-12s: 启用\n" "LBR模式"
    printf "\n"

    # 启动后台清理守护进程
    start_cleanup_daemon "$cleanup_secs" "$db_dir"

    # 启动性能分析工具到后台
    sudo nohup $cmd > "$DEFAULT_LOG_DIR/profiling_tool.log" 2>&1 &
    local profiling_pid=$!

    echo $profiling_pid > "$DEFAULT_LOG_DIR/profiling_tool.pid"

    if kill -0 $profiling_pid 2>/dev/null; then
        echo -e "${GREEN}✓ 性能分析已启动 (PID: $profiling_pid)${NC}"
    else
        echo -e "${RED}✗ 性能分析启动失败${NC}"
        return 1
    fi

    printf "\n"
    printf "${GREEN}性能分析正在后台运行${NC}\n"
    printf "  %-12s: %s\n" "日志文件" "${DEFAULT_LOG_DIR}/profiling_tool.log"
    printf "  %-12s: %s\n" "查询命令" "./smart_query.sh --time '15:10-15:30'"
    printf "  %-12s: %s\n" "停止命令" "sudo ./auto_manager.sh stop"
    printf "  %-12s: %s\n" "查看状态" "./auto_manager.sh status"
}

# 启动后台清理守护进程
start_cleanup_daemon() {
    local cleanup_secs="${1:-30}"
    local db_dir="${2:-$DEFAULT_DB_DIR}"

    # 使用nohup确保后台进程不受终端影响
    nohup bash -c "
        while true; do
            sleep $cleanup_secs
            $SCRIPT_DIR/auto_manager.sh manual-cleanup --dir \"$db_dir\" --days $DAYS_TO_KEEP >/dev/null 2>&1 || true
        done
    " >/dev/null 2>&1 &

    local daemon_pid=$!
    echo $daemon_pid > "$DEFAULT_LOG_DIR/cleanup_daemon.pid"

    # 确保进程确实在运行
    if kill -0 $daemon_pid 2>/dev/null; then
        echo -e "${GREEN}✓ 后台清理守护进程已启动 (PID: $daemon_pid)${NC}"
    else
        echo -e "${RED}✗ 后台清理守护进程启动失败${NC}"
        return 1
    fi
}

# 清理旧数据函数
cleanup_old_data() {
    local db_dir="$DEFAULT_DB_DIR"
    local days_to_keep="$DAYS_TO_KEEP"
    local dry_run=false

    while [[ $# -gt 0 ]]; do
        case $1 in
            --dir)
                db_dir="$2"
                shift 2
                ;;
            --days)
                days_to_keep="$2"
                shift 2
                ;;
            --dry-run)
                dry_run=true
                shift
                ;;
            *)
                echo -e "${RED}错误: 未知参数 '$1'${NC}"
                exit 1
                ;;
        esac
    done

    if [[ ! -d "$db_dir" ]]; then
        echo -e "${YELLOW}警告: 目录 '$db_dir' 不存在${NC}"
        return 0
    fi

    local cutoff_date=$(date -d "$days_to_keep days ago" +%Y-%m-%d)
    local cutoff_timestamp=$(date -d "$days_to_keep days ago" +%s)

    echo -e "${GREEN}清理配置${NC}:"
    echo "  目录: $db_dir"
    echo "  保留天数: $days_to_keep"
    echo "  截止时间: $cutoff_date"

    local files_to_delete=()
    while IFS= read -r -d '' file; do
        local file_date=$(basename "$file" | grep -oP 'profiling_\K[0-9]{4}-[0-9]{2}-[0-9]{2}')
        local file_timestamp=$(date -d "$file_date" +%s 2>/dev/null || echo 0)

        if [[ $file_timestamp -lt $cutoff_timestamp ]]; then
            files_to_delete+=("$file")
        fi
    done < <(find "$db_dir" -name "profiling_*.db" -type f -print0)

    local count=${#files_to_delete[@]}
    if [[ $count -eq 0 ]]; then
        echo -e "${GREEN}无需清理，没有找到超过 $days_to_keep 天的文件${NC}"
        return 0
    fi

    if [[ "$dry_run" == true ]]; then
        echo -e "${YELLOW}以下文件将被删除 (干运行):${NC}"
        printf '%s\n' "${files_to_delete[@]}"
    else
        echo -e "${YELLOW}正在删除 $count 个旧文件...${NC}"
        for file in "${files_to_delete[@]}"; do
            echo "删除: $(basename "$file")"
            rm -f "$file"
        done
        echo -e "${GREEN}清理完成，删除了 $count 个文件${NC}"
    fi
}

# 停止性能分析
stop_profiling() {
    echo -e "${YELLOW}停止性能分析...${NC}"

    local stopped=false

    # 停止性能分析工具
    local pids=$(pgrep -f "$PROFILING_TOOL" || true)
    if [[ -n "$pids" ]]; then
        echo -e "${YELLOW}正在终止性能分析进程: $pids${NC}"

        # 尝试优雅终止
        echo "$pids" | xargs kill 2>/dev/null || true
        sleep 2

        # 检查残留进程并强制终止
        local remaining_pids=$(pgrep -f "$PROFILING_TOOL" || true)
        if [[ -n "$remaining_pids" ]]; then
            echo -e "${RED}警告: 仍有性能分析进程残留，强制终止...${NC}"
            echo "$remaining_pids" | xargs kill -9 2>/dev/null || true
            sleep 0.5
        fi
        echo -e "${GREEN}✓ 性能分析已停止${NC}"
        stopped=true
    else
        echo -e "${YELLOW}性能分析未运行${NC}"
    fi

    # 停止清理守护进程
    if [[ -f "$DEFAULT_LOG_DIR/cleanup_daemon.pid" ]]; then
        local pid=$(cat "$DEFAULT_LOG_DIR/cleanup_daemon.pid")
        if kill "$pid" 2>/dev/null; then
            echo -e "${GREEN}✓ 清理进程已停止 (PID: $pid)${NC}"
            sleep 0.5
            # 再次确认进程已停止
            if kill -0 "$pid" 2>/dev/null; then
                kill -9 "$pid" 2>/dev/null || true
            fi
        else
            echo -e "${YELLOW}清理进程 (PID: $pid) 已停止或未运行${NC}"
        fi
        rm -f "$DEFAULT_LOG_DIR/cleanup_daemon.pid"
        stopped=true
    else
        echo -e "${YELLOW}清理进程未运行${NC}"
    fi

    # 清理PID文件
    rm -f "$DEFAULT_LOG_DIR/profiling_tool.pid" "$DEFAULT_LOG_DIR/cleanup_daemon.pid"

    if [[ "$stopped" == true ]]; then
        echo -e "${GREEN}所有相关服务已停止${NC}"
    else
        echo -e "${YELLOW}没有运行的服务需要停止${NC}"
    fi
}


# 查看状态
show_status() {
    echo -e "${GREEN}性能分析系统状态${NC}"
    echo "========================"

    # 检查性能分析进程
    local profiling_pid
    profiling_pid=$(pgrep -f "profiling_tool" | head -1 || true)
    if [[ -n "$profiling_pid" ]]; then
        echo -e "${GREEN}✓ 性能分析: 运行中 (PID: $profiling_pid)${NC}"
    else
        echo -e "${RED}✗ 性能分析: 未运行${NC}"
    fi

    # 检查清理进程
    if [[ -f "$SCRIPT_DIR/log/cleanup_daemon.pid" ]]; then
        local pid=$(cat "$SCRIPT_DIR/log/cleanup_daemon.pid")
        if kill -0 "$pid" 2>/dev/null; then
            echo -e "${GREEN}✓ 后台清理: 运行中 (PID: $pid)${NC}"
        else
            echo -e "${RED}✗ 后台清理: 未运行${NC}"
        fi
    else
        echo -e "${RED}✗ 后台清理: 未运行${NC}"
    fi

    # 检查systemd定时任务
    if systemctl is-active --quiet "$TIMER_NAME.timer" 2>/dev/null; then
        echo -e "${GREEN}✓ systemd定时任务: 已启用${NC}"
        echo "  下次执行: $(systemctl show "$TIMER_NAME.timer" --property=NextElapseUSecRealtime --value)"
    else
        echo -e "${YELLOW}⚠ systemd定时任务: 未启用${NC}"
    fi

    # 显示数据目录使用情况
    if [[ -d "$SCRIPT_DIR/tmp" ]]; then
        local size=$(du -sh "$SCRIPT_DIR/tmp" 2>/dev/null | cut -f1)
        local count=$(find "$SCRIPT_DIR/tmp" -name "*.db" 2>/dev/null | wc -l)
        echo "  数据文件: $count 个 ($size)"
    fi
}

# 安装systemd定时任务
install_systemd() {
    echo -e "${GREEN}安装systemd定时任务...${NC}"

    # 复制服务文件到系统目录
    cp "$SCRIPT_DIR/profiling-cleanup.service" "/etc/systemd/system/"
    cp "$SCRIPT_DIR/profiling-cleanup.timer" "/etc/systemd/system/"
    systemctl daemon-reload

    # 启用并启动定时器
    systemctl enable "$TIMER_NAME.timer"
    systemctl start "$TIMER_NAME.timer"

    echo -e "${GREEN}定时任务已安装并启用${NC}"
    systemctl status "$TIMER_NAME.timer" --no-pager
}

# 卸载systemd定时任务
uninstall_systemd() {
    echo -e "${YELLOW}卸载systemd定时任务...${NC}"

    # 停止并禁用定时器
    systemctl stop "$TIMER_NAME.timer" 2>/dev/null || true
    systemctl disable "$TIMER_NAME.timer" 2>/dev/null || true

    # 删除服务文件
    rm -f "/etc/systemd/system/$SERVICE_NAME.service"
    rm -f "/etc/systemd/system/$TIMER_NAME.timer"

    # 重新加载systemd配置
    systemctl daemon-reload

    echo -e "${GREEN}定时任务已卸载${NC}"
}

# 手动清理
manual_cleanup() {
    check_requirements

    echo -e "${GREEN}执行手动清理...${NC}"
    cd "$SCRIPT_DIR"

    if [[ -f "./cleanup_profiles.sh" ]]; then
        ./cleanup_profiles.sh --dir "$SCRIPT_DIR/tmp" "$@"
    else
        echo -e "${RED}清理脚本未找到${NC}"
        exit 1
    fi
}

# 查看日志
show_logs() {
    echo -e "${GREEN}查看 profiling_tool 日志...${NC}"
    local log_file="$DEFAULT_LOG_DIR/profiling_tool.log"
    if [[ -f "$log_file" ]]; then
        tail -f "$log_file"
    else
        echo -e "${YELLOW}日志文件 '$log_file' 未找到${NC}"
    fi
}

# 主函数
main() {
    case "${1:-help}" in
        start)
            shift
            start_profiling "$@"
            ;;
        stop)
            stop_profiling
            ;;
        status)
            show_status
            ;;
        install)
            install_systemd
            ;;
        uninstall)
            uninstall_systemd
            ;;
        manual-cleanup)
            shift
            manual_cleanup "$@"
            ;;
        logs)
            show_logs
            ;;
        help|--help|-h)
            show_help
            ;;
        *)
            echo -e "${RED}错误: 未知命令 '$1'${NC}"
            show_help
            exit 1
            ;;
    esac
}

# 执行主函数
main "$@"