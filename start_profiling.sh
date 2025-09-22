#!/bin/bash
# 智能性能分析启动工具 - 集成清理、启动和采集

set -e

# 颜色输出
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

# 获取脚本的绝对路径
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# 默认配置
DEFAULT_DB_DIR="$SCRIPT_DIR/tmp"
DEFAULT_LOG_DIR="$SCRIPT_DIR/log"
DEFAULT_FREQUENCY=30

# 显示帮助
show_help() {
    cat << EOF
${GREEN}性能分析启动工具${NC}

用法: $0 [选项]

选项:
  --dir <路径>        数据库存储目录 (默认: $DEFAULT_DB_DIR)
  --frequency <Hz>     采样频率 (默认: $DEFAULT_FREQUENCY)
  --filter <模式>     显示过滤: user|kernel|all (默认: all)
  --cleanup <秒>      清理间隔 (默认: 30)
  --stack-depth <层数> 最大栈深度 (默认: 48)
  --lbr               启用LBR精确调用栈
  --help              显示此帮助

示例:
  $0                                    # 使用默认配置
  $0 --dir /home/user/profiling         # 指定存储目录
  $0 --frequency 100 --filter user      # 高频用户态采样
  $0 --lbr --stack-depth 64             # 启用LBR，64层栈
EOF
}

# 参数解析
DB_DIR="$DEFAULT_DB_DIR"
FREQUENCY="$DEFAULT_FREQUENCY"
FILTER="all"
CLEANUP="30"
STACK_DEPTH="48"
LBR=""

while [[ $# -gt 0 ]]; do
    case $1 in
        --dir=*) # 处理 --dir=PATH 格式
            DB_DIR="${1#*=}"
            shift
            ;;
        --dir) # 处理 --dir PATH 格式
            DB_DIR="$2"
            shift 2
            ;;
        --frequency=*) # 处理 --frequency=NUM 格式
            FREQUENCY="${1#*=}"
            shift
            ;;
        --frequency) # 处理 --frequency NUM 格式
            FREQUENCY="$2"
            shift 2
            ;;
        --filter)
            FILTER="$2"
            shift 2
            ;;
        --cleanup)
            CLEANUP="$2"
            shift 2
            ;;
        --stack-depth)
            STACK_DEPTH="$2"
            shift 2
            ;;
        --lbr)
            LBR="--lbr"
            shift
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

# 检查必需工具
echo -e "${YELLOW}检查必需工具...${NC}"
for tool in profiling_tool cleanup_profiles.sh smart_query.sh; do
    if [[ ! -f "$tool" ]]; then
        echo -e "${RED}错误: $tool 未找到${NC}"
        exit 1
    fi
done

# 清理旧数据
echo -e "${YELLOW}正在清理7天前的旧数据...${NC}"
./cleanup_profiles.sh --dir "$DB_DIR" 2>/dev/null || true

# 检查并创建目录
echo -e "${YELLOW}检查数据库目录: $DB_DIR${NC}"
mkdir -p "$DB_DIR"

echo -e "${YELLOW}检查日志目录: $DEFAULT_LOG_DIR${NC}"
mkdir -p "$DEFAULT_LOG_DIR"

# 构建启动命令
cmd="./profiling_tool --output-dir=$DB_DIR --frequency=$FREQUENCY --filter=$FILTER --cleanup=$CLEANUP --stack-depth=$STACK_DEPTH $LBR"

echo -e "${GREEN}启动配置:${NC}"
echo "  数据目录: $DB_DIR"
echo "  日志目录: $DEFAULT_LOG_DIR"
echo "  采样频率: $FREQUENCY Hz"
echo "  过滤模式: $FILTER"
echo "  清理间隔: $CLEANUP 秒"
echo "  最大栈深: $STACK_DEPTH 层"
[[ -n "$LBR" ]] && echo "  LBR模式: 启用"
echo ""

# 显示使用提示
echo -e "${BLUE}查询命令:${NC}"
echo "  ./smart_query.sh --time '15:10-15:30'"
echo "  ./smart_query.sh --time 'last 20 minutes' --pid 1234"
echo "  ./smart_query.sh --time 'today 09:00-10:30' --name nginx"
echo ""
echo -e "${GREEN}开始性能分析...按Ctrl+C停止${NC}"
echo ""

    # 启动性能分析
    sudo $cmd > "$DEFAULT_LOG_DIR/profiling_output.log" 2>&1