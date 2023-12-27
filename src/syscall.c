#include "utils.h"
#include <stdbool.h>

#include "types.h"
#include "common/arguments.h"
#include "common/common.h"
#include "common/consts.h"
#include "common/context.h"
#include "common/filtering.h"

typedef struct syscall_point_args_t {
    u32 nr;
    u32 count;
    point_arg point_args[MAX_POINT_ARG_COUNT];
    point_arg point_arg_ret;
} syscall_point_args;

// syscall_point_args_map 的 key 就是 nr
struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __type(key, u32);
    __type(value, struct syscall_point_args_t);
    __uint(max_entries, 512);
} syscall_point_args_map SEC(".maps");

static __always_inline u32 read_args(program_data_t p, point_args_t* point_args, op_ctx_t* op_ctx, struct pt_regs* regs) {
    int zero = 0;
    // op_config_t* op = NULL;
    op_config_t* op = bpf_map_lookup_elem(&op_list, &zero);
    for (int i = 0; i < MAX_OP_COUNT; i++) {
        if (op != NULL && op_ctx->post_code != OP_SKIP) {
            op_ctx->op_code = op_ctx->post_code;
            op_ctx->post_code = OP_SKIP;
        } else {
            if (op_ctx->op_key_index >= MAX_OP_COUNT) return 0;
            u32 op_key = point_args->op_key_list[op_ctx->op_key_index];
            // bpf_printk("[stackplz] op_key:%d\n", op_key);
            op = bpf_map_lookup_elem(&op_list, &op_key);
            if (unlikely(op == NULL)) return 0;
            op_ctx->op_code = op->code;
            op_ctx->post_code = op->post_code;
            op_ctx->op_key_index += 1;
        }
        // bpf_printk("[stackplz] index:%d value:%ld\n", op_ctx->op_key_index, op->value);
        // bpf_printk("[stackplz] code:%d pre_code:%d post_code:%d\n", op->code, op->pre_code, op->post_code);
        // bpf_printk("[stackplz] %d op_code:%d\n", i, op_ctx->op_code);
        if (op_ctx->op_code == OP_SKIP) break;
        switch (op_ctx->op_code) {
            case OP_RESET_CTX:
                op_ctx->break_count = 0;
                op_ctx->reg_index = 0;
                op_ctx->read_addr = 0;
                op_ctx->read_len = 0;
                op_ctx->reg_value = 0;
                op_ctx->pointer_value = 0;
                break;
            case OP_SET_REG_INDEX:
                op_ctx->reg_index = op->value;
                break;
            case OP_SET_READ_LEN:
                op_ctx->read_len = op->value;
                break;
            case OP_SET_READ_LEN_REG_VALUE:
                if (op_ctx->read_len > op_ctx->reg_value) {
                    op_ctx->read_len = op_ctx->reg_value;
                }
                break;
            case OP_SET_READ_LEN_POINTER_VALUE:
                // bpf_printk("[stackplz] OP_SET_READ_LEN_POINTER_VALUE old_len:%d new_len:%d\n", op_ctx->read_len, op_ctx->pointer_value);
                if (op_ctx->read_len > op_ctx->pointer_value) {
                    op_ctx->read_len = op_ctx->pointer_value;
                }
                break;
            case OP_SET_READ_COUNT:
                op_ctx->read_len *= op->value;
                break;
            case OP_ADD_OFFSET:
                // bpf_printk("[stackplz] OP_ADD_OFFSET ptr:0x%lx add:%d\n", op_ctx->read_addr, op->value);
                op_ctx->read_addr += op->value;
                break;
            case OP_SUB_OFFSET:
                // bpf_printk("[stackplz] OP_SUB_OFFSET ptr:0x%lx sub:%d\n", op_ctx->read_addr, op->value);
                op_ctx->read_addr -= op->value;
                break;
            case OP_MOVE_REG_VALUE:
                op_ctx->read_addr = op_ctx->reg_value;
                break;
            case OP_MOVE_POINTER_VALUE:
                op_ctx->read_addr = op_ctx->pointer_value;
                break;
            case OP_MOVE_TMP_VALUE:
                op_ctx->read_addr = op_ctx->tmp_value;
                break;
            case OP_SET_TMP_VALUE:
                op_ctx->tmp_value = op_ctx->read_addr;
                break;
            case OP_FOR_BREAK:
                if (op_ctx->loop_count == 0) {
                    op_ctx->loop_index = op_ctx->op_key_index;
                }
                if (op_ctx->loop_count >= op_ctx->break_count) {
                    op_ctx->loop_count = 0;
                    op_ctx->break_count = 0;
                    op_ctx->loop_index = 0;
                } else {
                    op_ctx->loop_count += 1;
                    op_ctx->op_key_index = op_ctx->loop_index;
                }
                break;
            case OP_SET_BREAK_COUNT:
                op_ctx->break_count = MAX_LOOP_COUNT;
                if (op_ctx->break_count > op->value) {
                    op_ctx->break_count = op->value;
                }
                break;
            case OP_SET_BREAK_COUNT_REG_VALUE:
                op_ctx->break_count = MAX_LOOP_COUNT;
                if (op_ctx->break_count > op_ctx->reg_value) {
                    op_ctx->break_count = op_ctx->reg_value;
                }
                break;
            case OP_SET_BREAK_COUNT_POINTER_VALUE:
                op_ctx->break_count = MAX_LOOP_COUNT;
                if (op_ctx->break_count > op_ctx->pointer_value) {
                    op_ctx->break_count = op_ctx->pointer_value;
                }
                break;
            case OP_SAVE_ADDR:
                // bpf_printk("[stackplz] OP_SAVE_ADDR val:0x%lx idx:%d\n", op_ctx->read_addr, op_ctx->save_index);
                save_to_submit_buf(p.event, (void *)&op_ctx->read_addr, sizeof(op_ctx->read_addr), op_ctx->save_index);
                op_ctx->save_index += 1;
                break;
            case OP_READ_REG:
                if (op->pre_code == OP_SET_REG_INDEX) {
                    op_ctx->reg_index = op->value;
                }
                // make ebpf verifier happy
                if (op_ctx->reg_index >= REG_ARM64_MAX) {
                    return 0;
                }
                if (op_ctx->reg_index == 0) {
                    op_ctx->reg_value = op_ctx->reg_0;
                } else {
                    op_ctx->reg_value = READ_KERN(regs->regs[op_ctx->reg_index]);
                }
                break;
            case OP_SAVE_REG:
                save_to_submit_buf(p.event, (void *)&op_ctx->reg_value, sizeof(op_ctx->reg_value), op_ctx->save_index);
                op_ctx->save_index += 1;
                break;
            case OP_READ_POINTER:
                if (op->pre_code == OP_ADD_OFFSET) {
                    bpf_probe_read_user(&op_ctx->pointer_value, sizeof(op_ctx->pointer_value), (void*)(op_ctx->read_addr + op->value));
                } else if (op->pre_code == OP_SUB_OFFSET) {
                    bpf_probe_read_user(&op_ctx->pointer_value, sizeof(op_ctx->pointer_value), (void*)(op_ctx->read_addr - op->value));
                } else {
                    bpf_probe_read_user(&op_ctx->pointer_value, sizeof(op_ctx->pointer_value), (void*)op_ctx->read_addr);
                }
                break;
            case OP_SAVE_POINTER:
                save_to_submit_buf(p.event, (void *)&op_ctx->pointer_value, sizeof(op_ctx->pointer_value), op_ctx->save_index);
                op_ctx->save_index += 1;
                break;
            case OP_SAVE_STRUCT:
                // fix memory tag
                op_ctx->read_addr = op_ctx->read_addr & 0xffffffffff;
                if (op->pre_code == OP_SET_READ_COUNT) {
                    op_ctx->read_len *= op->value;
                }
                if (op_ctx->read_len > MAX_BYTES_ARR_SIZE) {
                    op_ctx->read_len = MAX_BYTES_ARR_SIZE;
                }
                // bpf_printk("[stackplz] OP_SAVE_STRUCT ptr:0x%lx len:%d\n", op_ctx->read_addr, op_ctx->read_len);
                int save_struct_status = save_bytes_to_buf(p.event, (void *)(op_ctx->read_addr), op_ctx->read_len, op_ctx->save_index);
                if (save_struct_status == 0) {
                    // 保存失败的情况 比如是一个非法的地址 那么就填一个空的 buf
                    // 那么只会保存 save_index 和 size -> [save_index][size][]
                    // ? 这里的处理方法好像不对 应该没问题 因为失败的时候 buf_off 没有变化
                    save_bytes_to_buf(p.event, 0, 0, op_ctx->save_index);
                }
                op_ctx->save_index += 1;
                break;
            case OP_SAVE_STRING:
                // fix memory tag
                op_ctx->read_addr = op_ctx->read_addr & 0xffffffffff;
                int save_string_status = save_str_to_buf(p.event, (void*) op_ctx->read_addr, op_ctx->save_index);
                if (save_string_status == 0) {
                    // 失败的情况存一个空数据 暂时没有遇到 有待测试
                    save_bytes_to_buf(p.event, 0, 0, op_ctx->save_index);
                }
                op_ctx->save_index += 1;
                break;
            case OP_SAVE_PTR_STRING:
            {
                u64 ptr = op_ctx->read_addr & 0xffffffffff;
                bpf_probe_read_user(&ptr, sizeof(ptr), (void*) ptr);
                save_to_submit_buf(p.event, (void *)&ptr, sizeof(ptr), op_ctx->save_index);
                // 每次取出后使用前都要 fix 很坑
                ptr = ptr & 0xffffffffff;
                int status = save_str_to_buf(p.event, (void*) ptr, op_ctx->save_index);
                if (status == 0) {
                    save_bytes_to_buf(p.event, 0, 0, op_ctx->save_index);
                    // 为读取字符串数组设计的
                    op_ctx->loop_count = op_ctx->break_count;
                }
                op_ctx->save_index += 1;
                break;
            }
            default:
                // bpf_printk("[stackplz] unknown op code:%d\n", op->code);
                break;
        }
    }
    return 0;
}

SEC("raw_tracepoint/sched_process_fork")
int tracepoint__sched__sched_process_fork(struct bpf_raw_tracepoint_args *ctx)
{
    long ret = 0;
    program_data_t p = {};
    if (!init_program_data(&p, ctx))
        return 0;

    struct task_struct *parent = (struct task_struct *) ctx->args[0];
    struct task_struct *child = (struct task_struct *) ctx->args[1];

    // 为了实现仅指定单个pid时 能追踪其产生的子进程的相关系统调用 设计如下
    // 维护一个 map
    // - 其 key 为进程 pid 
    // - 其 value 为其父进程 pid
    // 逻辑如下
    // 当进入此处后，先获取进程本身信息，然后通过自己的父进程 pid 去 map 中取出对应的value
    // 如果没有取到则说明这个进程不是要追踪的进程
    // 取到了，则说明这个是之前产生的进程，然后向map存入进程信息 key 就是进程本身 pid 而 value则是父进程pid
    // 那么最开始的 pid 从哪里来呢 答案是从首次通过 sys_enter 的过滤之后 向该map存放第一个key value
    // 1. child_parent_map => {}
    // 2. 出现第一个通过 sys_enter 处的过滤的进程，则更新map -> child_parent_map => {12345: 12345}
    // 3. sched_process_fork 获取进程的父进程信息，检查map，发现父进程存在其中，则更新map -> child_parent_map => {12345: 12345, 22222: 12345}
    // 4. sys_enter/sys_exit 有限次遍历 child_parent_map 取出key逐个比较当前进程的pid
    // 待实现...

    u32 parent_ns_pid = get_task_ns_pid(parent);
    u32 parent_ns_tgid = get_task_ns_tgid(parent);
    u32 child_ns_pid = get_task_ns_pid(child);
    u32 child_ns_tgid = get_task_ns_tgid(child);

    // bpf_printk("[syscall] parent_ns_pid:%d child_ns_pid:%d\n", parent_ns_pid, child_ns_pid);
    u32* pid = bpf_map_lookup_elem(&child_parent_map, &parent_ns_pid);
    if (pid == NULL) {
        return 0;
    }
    if (*pid == parent_ns_pid){
        // map中取出的父进程pid 这里fork产生子进程的pid相同
        // 说明这个进程是我们自己添加的
        // 那么现在把新产生的这个子进程 pid 放入 map
        ret = bpf_map_update_elem(&child_parent_map, &child_ns_pid, &parent_ns_pid, BPF_ANY);
        // bpf_printk("[syscall] parent_ns_pid:%d child_ns_pid:%d ret:%ld\n", parent_ns_pid, child_ns_pid, ret);
    } else {
        // 理论上不应该走到这个分支
        // 因为我们用当前函数这里的 parent 期望的就是其之前map中的
        bpf_printk("[syscall] parent pid from map:%d\n", *pid);
    }

    return 0;
}

SEC("raw_tracepoint/sys_enter")
int next_raw_syscalls_sys_enter(struct bpf_raw_tracepoint_args* ctx) {
    program_data_t p = {};
    if (!init_program_data(&p, ctx))
        return 0;

    if (!should_trace(&p))
        return 0;

    struct pt_regs *regs = (struct pt_regs *)(ctx->args[0]);
    u64 syscallno = READ_KERN(regs->syscallno);
    u32 sysno = (u32)syscallno;
    // 先根据调用号确定有没有对应的参数获取方案 没有直接结束
    point_args_t* point_args = bpf_map_lookup_elem(&sysenter_point_args, &sysno);
    if (point_args == NULL) {
        // bpf_printk("[syscall] unsupport nr:%d\n", sysno);
        return 0;
    }

    u32 filter_key = 0;
    common_filter_t* filter = bpf_map_lookup_elem(&common_filter, &filter_key);
    if (filter == NULL) {
        return 0;
    }

    if (filter->trace_mode == TRACE_COMMON) {
        // 非 追踪全部syscall模式
        u32 sysno_whitelist_key = sysno + SYS_WHITELIST_START;
        u32 *sysno_whitelist_value = bpf_map_lookup_elem(&common_list, &sysno_whitelist_key);
        if (sysno_whitelist_value == NULL) {
            return 0;
        }
    }

    // 黑名单同样对 追踪全部syscall模式 有效
    u32 sysno_blacklist_key = sysno + SYS_BLACKLIST_START;
    u32 *sysno_blacklist_value = bpf_map_lookup_elem(&common_list, &sysno_blacklist_key);
    if (sysno_blacklist_value != NULL) {
        return 0;
    }

    // 保存寄存器应该放到所有过滤完成之后
    args_t saved_regs = {};
    saved_regs.args[0] = READ_KERN(regs->regs[0]);
    saved_regs.args[1] = READ_KERN(regs->regs[1]);
    saved_regs.args[2] = READ_KERN(regs->regs[2]);
    saved_regs.args[3] = READ_KERN(regs->regs[3]);
    saved_regs.args[4] = READ_KERN(regs->regs[4]);
    saved_regs.args[5] = READ_KERN(regs->regs[5]);
    save_args(&saved_regs, SYSCALL_ENTER);

    // event->context 已经有进程的信息了
    save_to_submit_buf(p.event, (void *) &sysno, sizeof(u32), 0);

    // 先获取 lr sp pc 并发送 这样可以尽早计算调用来源情况
    // READ_KERN 好像有问题
    u64 lr = 0;
    if(filter->is_32bit) {
        bpf_probe_read_kernel(&lr, sizeof(lr), &regs->regs[14]);
        save_to_submit_buf(p.event, (void *) &lr, sizeof(u64), 1);
    }
    else {
        bpf_probe_read_kernel(&lr, sizeof(lr), &regs->regs[30]);
        save_to_submit_buf(p.event, (void *) &lr, sizeof(u64), 1);
    }
    u64 pc = 0;
    u64 sp = 0;
    bpf_probe_read_kernel(&pc, sizeof(pc), &regs->pc);
    bpf_probe_read_kernel(&sp, sizeof(sp), &regs->sp);
    save_to_submit_buf(p.event, (void *) &pc, sizeof(u64), 2);
    save_to_submit_buf(p.event, (void *) &sp, sizeof(u64), 3);

    int ctx_index = 0;
    op_ctx_t* op_ctx = bpf_map_lookup_elem(&op_ctx_map, &ctx_index);
    // make ebpf verifier happy
    if (unlikely(op_ctx == NULL)) return 0;
    __builtin_memset((void *)op_ctx, 0, sizeof(op_ctx));

    op_ctx->reg_0 = saved_regs.args[0];
    op_ctx->save_index = 4;
    op_ctx->op_key_index = 0;

    read_args(p, point_args, op_ctx, regs);

    events_perf_submit(&p, SYSCALL_ENTER);
    if (filter->signal > 0) {
        bpf_send_signal(filter->signal);
    }
    return 0;
}

SEC("raw_tracepoint/sys_exit")
int next_raw_syscalls_sys_exit(struct bpf_raw_tracepoint_args* ctx) {

    program_data_t p = {};
    if (!init_program_data(&p, ctx))
        return 0;

    if (!should_trace(&p))
        return 0;

    struct pt_regs *regs = (struct pt_regs *)(ctx->args[0]);
    u64 syscallno = READ_KERN(regs->syscallno);
    u32 sysno = (u32)syscallno;

    point_args_t* point_args = bpf_map_lookup_elem(&sysexit_point_args, &sysno);
    if (point_args == NULL) {
        return 0;
    }

    u32 filter_key = 0;
    common_filter_t* filter = bpf_map_lookup_elem(&common_filter, &filter_key);
    if (filter == NULL) {
        return 0;
    }

    args_t saved_regs;
    if (load_args(&saved_regs, SYSCALL_ENTER) != 0) {
        return 0;
    }
    del_args(SYSCALL_ENTER);
    if (saved_regs.flag == 1) {
        return 0;
    }

    if (filter->trace_mode == TRACE_COMMON) {
        // 非 追踪全部syscall模式
        u32 sysno_whitelist_key = sysno + SYS_WHITELIST_START;
        u32 *sysno_whitelist_value = bpf_map_lookup_elem(&common_list, &sysno_whitelist_key);
        if (sysno_whitelist_value == NULL) {
            return 0;
        }
    }

    // 黑名单同样对 追踪全部syscall模式 有效
    u32 sysno_blacklist_key = sysno + SYS_BLACKLIST_START;
    u32 *sysno_blacklist_value = bpf_map_lookup_elem(&common_list, &sysno_blacklist_key);
    if (sysno_blacklist_value != NULL) {
        return 0;
    }

    // 保存系统调用号
    save_to_submit_buf(p.event, (void *) &sysno, sizeof(u32), 0);

    int ctx_index = 1;
    op_ctx_t* op_ctx = bpf_map_lookup_elem(&op_ctx_map, &ctx_index);
    if (unlikely(op_ctx == NULL)) return 0;
    __builtin_memset((void *)op_ctx, 0, sizeof(op_ctx));

    op_ctx->reg_0 = saved_regs.args[0];
    op_ctx->save_index = 1;
    op_ctx->op_key_index = 0;

    read_args(p, point_args, op_ctx, regs);

    // 读取返回值
    u64 ret = READ_KERN(regs->regs[0]);
    save_to_submit_buf(p.event, (void *) &ret, sizeof(ret), op_ctx->save_index);

    events_perf_submit(&p, SYSCALL_EXIT);
    return 0;
}

SEC("raw_tracepoint/sys_enter")
int raw_syscalls_sys_enter(struct bpf_raw_tracepoint_args* ctx) {

    // 除了实现对指定进程的系统调用跟踪 也要将其产生的子进程 加入追踪范围
    // 为了实现这个目的 fork 系统调用结束之后 应当检查其 父进程是否归属于当前被追踪的进程

    program_data_t p = {};
    if (!init_program_data(&p, ctx))
        return 0;

    if (!should_trace(&p))
        return 0;

    struct pt_regs *regs = (struct pt_regs *)(ctx->args[0]);
    u64 syscallno = READ_KERN(regs->syscallno);
    u32 sysno = (u32)syscallno;
    // 先根据调用号确定有没有对应的参数获取方案 没有直接结束
    struct syscall_point_args_t* syscall_point_args = bpf_map_lookup_elem(&syscall_point_args_map, &sysno);
    if (syscall_point_args == NULL) {
        // bpf_printk("[syscall] unsupport nr:%d\n", sysno);
        return 0;
    }

    u32 filter_key = 0;
    common_filter_t* filter = bpf_map_lookup_elem(&common_filter, &filter_key);
    if (filter == NULL) {
        return 0;
    }

    if (filter->trace_mode == TRACE_COMMON) {
        // 非 追踪全部syscall模式
        u32 sysno_whitelist_key = sysno + SYS_WHITELIST_START;
        u32 *sysno_whitelist_value = bpf_map_lookup_elem(&common_list, &sysno_whitelist_key);
        if (sysno_whitelist_value == NULL) {
            return 0;
        }
    }

    // 黑名单同样对 追踪全部syscall模式 有效
    u32 sysno_blacklist_key = sysno + SYS_BLACKLIST_START;
    u32 *sysno_blacklist_value = bpf_map_lookup_elem(&common_list, &sysno_blacklist_key);
    if (sysno_blacklist_value != NULL) {
        return 0;
    }

    // 保存寄存器应该放到所有过滤完成之后
    args_t args = {};
    args.args[0] = READ_KERN(regs->regs[0]);
    args.args[1] = READ_KERN(regs->regs[1]);
    args.args[2] = READ_KERN(regs->regs[2]);
    args.args[3] = READ_KERN(regs->regs[3]);
    args.args[4] = READ_KERN(regs->regs[4]);
    args.args[5] = READ_KERN(regs->regs[5]);
    save_args(&args, SYSCALL_ENTER);

    // event->context 已经有进程的信息了
    save_to_submit_buf(p.event, (void *) &sysno, sizeof(u32), 0);

    // 先获取 lr sp pc 并发送 这样可以尽早计算调用来源情况
    // READ_KERN 好像有问题
    u64 lr = 0;
    if(filter->is_32bit) {
        bpf_probe_read_kernel(&lr, sizeof(lr), &regs->regs[14]);
        save_to_submit_buf(p.event, (void *) &lr, sizeof(u64), 1);
    }
    else {
        bpf_probe_read_kernel(&lr, sizeof(lr), &regs->regs[30]);
        save_to_submit_buf(p.event, (void *) &lr, sizeof(u64), 1);
    }
    u64 pc = 0;
    u64 sp = 0;
    bpf_probe_read_kernel(&pc, sizeof(pc), &regs->pc);
    bpf_probe_read_kernel(&sp, sizeof(sp), &regs->sp);
    save_to_submit_buf(p.event, (void *) &pc, sizeof(u64), 2);
    save_to_submit_buf(p.event, (void *) &sp, sizeof(u64), 3);

    u32 point_arg_count = MAX_POINT_ARG_COUNT;
    if (syscall_point_args->count <= point_arg_count) {
        point_arg_count = syscall_point_args->count;
    }

    u32 next_arg_index = 4;
    u64 reg_0 = READ_KERN(regs->regs[0]);
    for (int i = 0; i < point_arg_count; i++) {
        struct point_arg_t* point_arg = (struct point_arg_t*) &syscall_point_args->point_args[i];
        if (point_arg->read_index == REG_ARM64_MAX) {
            continue;
        }
        u64 arg_ptr = get_arg_ptr(regs, point_arg, i, reg_0);

        // 先保存参数值本身
        save_to_submit_buf(p.event, (void *)&arg_ptr, sizeof(u64), (u8)next_arg_index);
        next_arg_index += 1;

        if (point_arg->point_flag != SYS_ENTER) {
            continue;
        }
        if (arg_ptr == 0) {
            continue;
        }
        u32 read_count = get_read_count(regs, point_arg);
        next_arg_index = read_arg(p, point_arg, arg_ptr, read_count, next_arg_index);
        if (point_arg->tmp_index == FILTER_INDEX_SKIP) {
            point_arg->tmp_index = 0;
            args.flag = 1;
            save_args(&args, SYSCALL_ENTER);
            return 0;
        }
    }
    events_perf_submit(&p, SYSCALL_ENTER);
    if (filter->signal > 0) {
        bpf_send_signal(filter->signal);
    }
    return 0;
}

SEC("raw_tracepoint/sys_exit")
int raw_syscalls_sys_exit(struct bpf_raw_tracepoint_args* ctx) {

    program_data_t p = {};
    if (!init_program_data(&p, ctx))
        return 0;

    if (!should_trace(&p))
        return 0;

    struct pt_regs *regs = (struct pt_regs *)(ctx->args[0]);
    u64 syscallno = READ_KERN(regs->syscallno);
    u32 sysno = (u32)syscallno;

    struct syscall_point_args_t* syscall_point_args = bpf_map_lookup_elem(&syscall_point_args_map, &sysno);
    if (syscall_point_args == NULL) {
        return 0;
    }

    u32 filter_key = 0;
    common_filter_t* filter = bpf_map_lookup_elem(&common_filter, &filter_key);
    if (filter == NULL) {
        return 0;
    }

    args_t saved_args;
    if (load_args(&saved_args, SYSCALL_ENTER) != 0) {
        return 0;
    }
    del_args(SYSCALL_ENTER);
    if (saved_args.flag == 1) {
        return 0;
    }

    if (filter->trace_mode == TRACE_COMMON) {
        // 非 追踪全部syscall模式
        u32 sysno_whitelist_key = sysno + SYS_WHITELIST_START;
        u32 *sysno_whitelist_value = bpf_map_lookup_elem(&common_list, &sysno_whitelist_key);
        if (sysno_whitelist_value == NULL) {
            return 0;
        }
    }

    // 黑名单同样对 追踪全部syscall模式 有效
    u32 sysno_blacklist_key = sysno + SYS_BLACKLIST_START;
    u32 *sysno_blacklist_value = bpf_map_lookup_elem(&common_list, &sysno_blacklist_key);
    if (sysno_blacklist_value != NULL) {
        return 0;
    }

    u32 next_arg_index = 0;
    save_to_submit_buf(p.event, (void *) &sysno, sizeof(u32), (u8)next_arg_index);
    next_arg_index += 1;

    u32 point_arg_count = MAX_POINT_ARG_COUNT;
    if (syscall_point_args->count <= point_arg_count) {
        point_arg_count = syscall_point_args->count;
    }
    u64 reg_0 = saved_args.args[0];
    for (int i = 0; i < point_arg_count; i++) {
        struct point_arg_t* point_arg = (struct point_arg_t*) &syscall_point_args->point_args[i];
        if (point_arg->read_index == REG_ARM64_MAX) {
            continue;
        }
        u64 arg_ptr = get_arg_ptr(regs, point_arg, i, reg_0);

        // 先保存参数值本身
        save_to_submit_buf(p.event, (void *)&arg_ptr, sizeof(u64), (u8)next_arg_index);
        next_arg_index += 1;

        if (point_arg->point_flag != SYS_EXIT) {
            continue;
        }
        if (arg_ptr == 0) {
            continue;
        }
        u32 read_count = get_read_count(regs, point_arg);
        next_arg_index = read_arg(p, point_arg, arg_ptr, read_count, next_arg_index);
    }

    // 读取返回值
    u64 ret = READ_KERN(regs->regs[0]);
    // 保存之
    save_to_submit_buf(p.event, (void *) &ret, sizeof(ret), (u8)next_arg_index);
    next_arg_index += 1;
    // 取返回值的参数配置 并尝试进一步读取
    struct point_arg_t* point_arg = (struct point_arg_t*) &syscall_point_args->point_arg_ret;
    next_arg_index = read_arg(p, point_arg, ret, 0, next_arg_index);
    // 发送数据
    events_perf_submit(&p, SYSCALL_EXIT);
    return 0;
}

// bpf_printk debug use
// echo 1 > /sys/kernel/tracing/tracing_on
// cat /sys/kernel/tracing/trace_pipe