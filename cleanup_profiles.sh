#!/bin/bash
# 7天过期清理脚本

set -e

# 配置
DEFAULT_DB_DIR="/tmp/profiling"
DAYS_TO_KEEP=7

# 颜色输出
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
RED='\033[0;31m'
NC='\033[0m'

# 显示帮助
show_help() {
    cat << EOF
${GREEN}数据库清理工具${NC}

用法: $0 [选项]

选项:
  --dir <目录>    数据库目录 (默认: $DEFAULT_DB_DIR)
  --days <天数>   保留天数 (默认: $DAYS_TO_KEEP)
  --dry-run      只显示要删除的文件，不实际删除
  --help         显示此帮助
EOF
}

# 主清理函数
cleanup_old_profiles() {
    local db_dir="$DEFAULT_DB_DIR"
    local days_to_keep=$DAYS_TO_KEEP
    local dry_run=false

    # 解析参数
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

    # 检查目录
    if [[ ! -d "$db_dir" ]]; then
        echo -e "${YELLOW}警告: 目录 '$db_dir' 不存在${NC}"
        exit 0
    fi

    # 计算截止时间
    local cutoff_date=$(date -d "$days_to_keep days ago" +%Y-%m-%d)
    local cutoff_timestamp=$(date -d "$days_to_keep days ago" +%s)

    echo -e "${GREEN}清理配置${NC}:"
    echo "  目录: $db_dir"
    echo "  保留天数: $days_to_keep"
    echo "  截止时间: $cutoff_date"

    # 查找旧文件
    local files_to_delete=()
    while IFS= read -r -d '' file; do
        local file_date=$(basename "$file" | sed 's/profiling_\([0-9-]*\)_[0-9]*\.db/\1/')
        local file_timestamp=$(date -d "$file_date" +%s 2>/dev/null || echo 0)

        if [[ $file_timestamp -lt $cutoff_timestamp ]]; then
            files_to_delete+=("$file")
        fi
    done < <(find "$db_dir" -name "profiling_*.db" -type f -print0)

    # 显示和清理
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

# 执行主函数
cleanup_old_profiles "$@" && exit 0 || exit 1