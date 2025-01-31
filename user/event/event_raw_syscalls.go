package event

import (
    "encoding/binary"
    "encoding/json"
    "fmt"
    "stackplz/user/config"
    "stackplz/user/util"
)

type SyscallEvent struct {
    ContextEvent
    UUID         string
    RegsBuffer   RegsBuf
    UnwindBuffer UnwindBuf
    nr_point     *config.SyscallPoint
    nr           config.Arg_nr
    lr           config.Arg_reg
    sp           config.Arg_reg
    pc           config.Arg_reg
    ret          uint64
    arg_str      string
}

func (this *SyscallEvent) ParseEvent() (IEventStruct, error) {
    data_e, err := this.ContextEvent.ParseEvent()
    if err != nil {
        panic("...")
    }
    if data_e == nil {
        if err := this.ParseContext(); err != nil {
            panic(fmt.Sprintf("SyscallEvent.ParseContext() err:%v", err))
        }
        return this, nil
    }
    return data_e, nil
}

func (this *SyscallEvent) ParseContext() (err error) {
    // 处理参数 常规参数的构成 是 索引 + 值
    if err = binary.Read(this.buf, binary.LittleEndian, &this.nr); err != nil {
        panic(err)
    }
    this.nr_point = config.GetSyscallPointByNR(this.nr.Value)

    // this.logger.Printf("ParseContext EventId:%d RawSample:\n%s", this.EventId, util.HexDump(this.rec.RawSample, util.COLORRED))

    if this.EventId == SYSCALL_ENTER {
        if err = binary.Read(this.buf, binary.LittleEndian, &this.lr); err != nil {
            panic(err)
        }
        if err = binary.Read(this.buf, binary.LittleEndian, &this.sp); err != nil {
            panic(err)
        }
        if err = binary.Read(this.buf, binary.LittleEndian, &this.pc); err != nil {
            panic(err)
        }
        this.arg_str = this.nr_point.ParseEnterPoint(this.buf)
    } else if this.EventId == SYSCALL_EXIT {
        this.arg_str = this.nr_point.ParseExitPoint(this.buf)
    } else {
        panic(fmt.Sprintf("SyscallEvent.ParseContext() failed, EventId:%d", this.EventId))
    }
    this.ParsePadding()
    err = this.ParseContextStack()
    if err != nil {
        panic(fmt.Sprintf("ParseContextStack err:%v", err))
    }
    return nil
}

func (this *SyscallEvent) GetUUID() string {
    s := fmt.Sprintf("%d|%d|%s", this.Pid, this.Tid, util.B2STrim(this.Comm[:]))
    if this.mconf.ShowTime {
        s = fmt.Sprintf("%d|%s", this.Ts, s)
    }
    if this.mconf.ShowUid {
        s = fmt.Sprintf("%d|%s", this.Uid, s)
    }
    return s
}

func (this *SyscallEvent) JsonString(stack_str string) string {
    if this.EventId == SYSCALL_ENTER {
        v := config.SyscallFmt{}
        v.Ts = this.Ts
        v.Event = "sys_enter"
        v.HostTid = this.HostTid
        v.HostPid = this.HostPid
        v.Tid = this.Tid
        v.Pid = this.Pid
        v.Uid = this.Uid
        v.Comm = util.B2STrim(this.Comm[:])
        v.Argnum = this.Argnum
        v.Stack = stack_str
        v.NR = this.nr_point.Name
        v.LR = fmt.Sprintf("0x%x", this.lr.Address)
        v.SP = fmt.Sprintf("0x%x", this.sp.Address)
        v.PC = fmt.Sprintf("0x%x", this.pc.Address)
        v.Arg_str = this.arg_str
        data, err := json.Marshal(v)
        if err != nil {
            panic(err)
        }
        return string(data)
    } else {
        v := config.SyscallExitFmt{}
        v.Ts = this.Ts
        v.Event = "sys_exit"
        v.HostTid = this.HostTid
        v.HostPid = this.HostPid
        v.Tid = this.Tid
        v.Pid = this.Pid
        v.Uid = this.Uid
        v.Comm = util.B2STrim(this.Comm[:])
        v.Argnum = this.Argnum
        v.Stack = stack_str
        v.NR = this.nr_point.Name
        v.Arg_str = this.arg_str
        data, err := json.Marshal(v)
        if err != nil {
            panic(err)
        }
        return string(data)
    }
}

func (this *SyscallEvent) String() string {
    stack_str := ""
    if this.EventId == SYSCALL_ENTER {
        stack_str = this.GetStackTrace(stack_str)
    }
    // if this.mconf.FmtJson {
    //     return this.JsonString(stack_str)
    // }
    var base_str string
    base_str = fmt.Sprintf("[%s] %s%s", this.GetUUID(), this.nr_point.Name, this.arg_str)
    if this.EventId == SYSCALL_ENTER {
        var lr_str string
        var pc_str string
        if this.mconf.GetOff {
            lr_str = fmt.Sprintf("LR:0x%x(%s)", this.lr.Address, this.GetOffset(this.lr.Address))
            pc_str = fmt.Sprintf("PC:0x%x(%s)", this.pc.Address, this.GetOffset(this.pc.Address))
        } else {
            lr_str = fmt.Sprintf("LR:0x%x", this.lr.Address)
            pc_str = fmt.Sprintf("PC:0x%x", this.pc.Address)
        }
        base_str = fmt.Sprintf("%s %s %s SP:0x%x", base_str, lr_str, pc_str, this.sp.Address)
    }
    return base_str + stack_str
}

func (this *SyscallEvent) Clone() IEventStruct {
    event := new(SyscallEvent)
    return event
}
