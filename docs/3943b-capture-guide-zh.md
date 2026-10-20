# 3943B IQ 采集经验交接说明

## 1. 文档目的

本文档不是本项目代码说明，而是将已经调试过的 3943B IQ 采集经验整理给其他软件开发人员，便于在新的软件中重新实现采集功能。

重点包括：

- 3943B 与 PC 的通信方式；
- VISA 和 SCPI 的使用方法；
- PC 端 IQ 数据接收方式；
- 已验证的采集命令和参数；
- IQRAW 数据包识别方法；
- 已经遇到的失败现象；
- 不能直接重复的高风险命令组合。

本文档描述的是当前调试设备和网络环境下的经验。不同固件版本、不同设备选件或不同前面板状态，可能导致命令行为不同。首次接入新软件时，应先做只读查询，再做小规模验证。

## 2. 已验证的通信拓扑

### 2.1 设备和 PC 地址

本次调试使用的网络拓扑如下：

```text
3943B IP：192.168.50.54
3943B VISA Socket：5025
PC IP：192.168.50.20
PC IQ 接收端口：5030
```

VISA 资源字符串：

```text
TCPIP::192.168.50.54::5025::SOCKET
```

PC 和 3943B 必须位于可互通的网络中。PC 端用于接收 IQ 数据的地址必须填写实际网卡地址，不能填写回环地址 `127.0.0.1`，也不能填写错误的网卡地址。

### 2.2 逻辑链路

采集功能实际上分为两条链路：

```text
PC --VISA/SCPI/TCP Socket--> 3943B
3943B --TCP 或 UDP IQ 数据--> PC
```

VISA 链路只负责控制仪器，IQ 数据不会通过 VISA 返回。IQ 数据由 3943B 根据 SCPI 配置，通过独立的 TCP 或 UDP 连接发送到 PC。

新的软件必须同时实现：

1. VISA 控制客户端；
2. PC 端 TCP Server 或 UDP Receiver；
3. IQRAW 文件保存；
4. 接收数据校验；
5. 停止采集和资源清理。

## 3. 推荐的实现流程

建议新软件按以下顺序实现，不要一开始就把所有参数和连续采集都接入。

### 阶段一：只读确认

先通过 VISA 连接设备，只发送查询命令，确认设备可通信：

```text
*IDN?
SYSTem:ERRor?
FREQuency:MODE?
FREQuency?
FREQuency:SPAN?
MEASure:MODE?
SYSTem:IF:REMote:MODE?
TRACe:IQ:DATA:REPort?
TRACe:PATH?
TRACe:UDP?
TRACe:TCP?
```

每次查询都应读取完整响应，并记录：

- 命令；
- 返回值；
- VISA 状态码；
- `SYSTem:ERRor?` 返回值；
- 设备是否仍然响应后续命令。

不要把 `SYSTem:ERRor? = 00` 当成“已经成功输出 IQ 数据”。本次调试中，某些命令虽然返回无错误，但设备实际没有发送数据。

### 阶段二：只配置 PC 上报路径

先只配置上报目标，不启动 IQ 数据流：

```text
TRACe:UDP:DEL ALL
TRACe:TCP:DEL ALL
TRACe:UDP:TAG "192.168.50.20",5030,IF
TRACe:UDP:FLAG "192.168.50.20",5030,"opt"
TRACe:PATH UDP
```

配置后查询确认：

```text
SYSTem:ERRor?
TRACe:UDP?
TRACe:PATH?
```

此阶段的目标是确认：

- 设备接受目标地址；
- 设备保存了 UDP/TCP 配置；
- SCPI 错误队列正常；
- 不启动数据流时设备保持稳定。

### 阶段三：启动 PC 接收器

在发送启动采集命令之前，PC 必须先开始监听：

```text
192.168.50.20:5030
```

接收器建议具备以下能力：

- UDP 模式使用较大的接收缓冲区；
- TCP 模式监听端口并接受设备连接；
- 接收到第一个字节后开始计时；
- 达到目标 IQ 点数后可以提前结束；
- 一段时间没有新数据时结束；
- 用户点击停止时立即关闭 socket；
- 保存失败时删除不完整文件；
- 记录接收字节数和接收时长。

本次实现中使用的默认策略：

```text
目标点数：由用户配置
首字节等待超时：约 3000 ms
数据空闲结束：约 300 ms
普通接收超时：约 5000 ms
```

### 阶段四：发送 IQ 触发命令

当前验证过的触发路径是“网络 IQ 报告”路径，不是仪器内部 IQ 文件记录路径。

典型命令序列如下：

```text
TRACe:UDP:DEL ALL
TRACe:TCP:DEL ALL
FREQuency:MODE FFM
INPut:ATTenuation:STATe AUTO
TRACe:IQ:DATA:REPort MAN
TRACe:RECord:SOURce IQAudio
FREQuency 2400MHz
BANDwidth 20MHz
SYSTem:IF:REMote:MODE SHORT
TRACe:IQ:DATA:NUMBer 4096
TRACe:PATH UDP
TRACe:UDP:FLAG "192.168.50.20",5030,"opt"
TRACe:UDP:TAG "192.168.50.20",5030,IF
TRIGger:IQ:REPort:STATe ON
```

说明：

- `2400MHz` 和 `20MHz` 只是示例，实际值由采集任务决定；
- `4096` 是示例 IQ 点数；
- TCP 模式需要把 `UDP` 命令替换为对应的 `TCP` 命令；
- `TRACe:IQ:DATA:NUMBer` 可根据设备手册和实际需求调整；
- `TRIGger:IQ:REPort:STATe ON` 是启动 IQ 报告的重要命令；
- `SYSTem:IF:REMote:MODE SHORT` 在当前调试设备上存在风险，见第 7 节。

新的软件应将命令写入后逐条检查 VISA 返回状态，并在关键阶段查询：

```text
SYSTem:ERRor?
```

但最终是否成功，必须以 PC 端是否收到有效 IF/IQ 数据为准。

### 阶段五：结束接收和停止上报

达到目标点数、超时或用户停止后，应执行：

```text
TRACe:UDP:DEL ALL
TRACe:TCP:DEL ALL
```

如果项目使用了额外的 IF Remote 或 IQ Report 状态命令，应根据设备手册和现场验证结果补充对应的关闭命令。

停止流程建议：

1. 先停止 PC 接收等待；
2. 保证接收到的数据已经 flush 到文件；
3. 发送设备停止或删除上报路径命令；
4. 查询 `SYSTem:ERRor?`；
5. 关闭 VISA 设备会话；
6. 将文件标记为完成或失败。

## 4. 参数建议

第一版集成建议只保留以下参数：

| 参数 | 示例 | 说明 |
|---|---:|---|
| VISA Resource | `TCPIP::192.168.50.54::5025::SOCKET` | 设备控制连接 |
| 中心频率 | `2400000000` | 单位 Hz |
| 频率跨度 | `20000000` | 单位 Hz |
| 解调方式 | `IQ` | IQ 采集使用 |
| 数据源 | `IQAudio` | 当前调试使用 |
| 上报路径 | `UDP` | 与 PC 接收协议一致 |
| PC IP | `192.168.50.20` | 接收端本机地址 |
| PC 端口 | `5030` | 监听端口 |
| IQ 点数 | `4096` | 建议先使用较小值 |
| 接收超时 | `5000 ms` | 无数据时退出 |

采集稳定后，再增加：

- TCP 模式；
- 连续采集；
- 自动重试；
- 更大 IQ 点数；
- CAP 转换；
- 采集任务队列。

## 5. IQRAW 数据格式经验

### 5.1 数据包识别

接收的原始字节流不是纯 IQ 数组，而是由 3943B 数据包头和负载组成。

当前解析器使用：

```text
数据包魔数：0x000EB200
IF 数据包标签：901
PSCAN 数据包标签：1201
```

数据包长度位于公共包头中。解析时应：

1. 在字节流中搜索 `0x000EB200`；
2. 读取数据包长度；
3. 判断当前缓冲区是否已经包含完整数据包；
4. 读取数据包标签；
5. 只处理标签为 `901` 的 IF 数据包；
6. 跳过固定包头和可选头；
7. 将剩余负载按 4 字节一组解析为一个 I/Q 点；
8. 每个 I、Q 分量为小端 `int16`。

伪代码：

```text
while buffer contains a complete packet:
    find magic 0x000EB200
    packetLength = read big-endian uint32 from header
    packetTag = read big-endian uint16 from header

    if packetTag == 901:
        optionalHeaderLength = read header field
        payloadOffset = fixedHeaderLength + optionalHeaderLength
        payloadLength = packetLength - payloadOffset

        for every 4 bytes in payload:
            I = little-endian int16
            Q = little-endian int16
            append (I, Q)

    remove this packet from buffer
```

### 5.2 有效文件判定

至少应检查：

- 文件大小大于 0；
- 能找到合法魔数；
- 至少存在一个标签为 `901` 的 IF 数据包；
- IF 负载长度大于 0；
- 负载长度能够按 4 字节对齐；
- 实际解析出的 IQ 点数大于 0。

如果只收到标签为 `1201` 的数据，应提示：

```text
收到的是 PSCAN 数据，不是 IF/IQ 数据。
```

### 5.3 UDP 丢包

UDP 采集可能存在丢包。新软件至少应记录：

- 收到的总字节数；
- 有效数据包数；
- IF 数据包数；
- 非 IF 数据包数；
- 解析出的 IQ 点数；
- 文件接收时长；
- 是否达到目标 IQ 点数。

不能只根据“收到了一些字节”就判定采集成功。

## 6. 当前调试中确认的关键经验

### 6.1 IQ 数据走网络，不走 VISA 读取

VISA 只用于控制 3943B。收到 IQ 数据要依靠 PC 端独立的 TCP/UDP 接收器。

错误思路：

```text
通过 viRead 直接读取完整 IQ 数据
```

正确思路：

```text
VISA 写入 SCPI
PC socket 监听 IQ 数据
从 TCP/UDP 接收原始字节
保存 IQRAW
```

### 6.2 不要使用仪器内部 IQ 记录路径代替 PC 接收

以下路径不是当前 PC IQRAW 采集的推荐方式：

```text
TRACe:RECord:SELectivity RECord
TRACe:RECord:SOURce IQ
TRACe:RECord:STATe ON
```

本次调试使用的是 IQ/IF 网络报告路径，原始数据由 PC 保存。

### 6.3 命令无错误不代表收到数据

设备可能返回：

```text
SYSTem:ERRor? = 00
```

但 PC 仍然收到 0 字节。因此采集成功条件必须同时包含：

```text
SCPI 命令无错误
PC 收到非零字节
收到有效 IF/IQ 数据包
解析出非零 IQ 点数
```

### 6.4 接收器必须先于启动命令

正确顺序：

```text
启动 PC TCP/UDP 接收器
打开 VISA 会话
配置设备
发送 IQ 报告启动命令
等待数据
```

如果先发送设备命令、再启动 PC 接收器，可能丢失最开始的数据，尤其是在 UDP 模式下。

## 7. 已知风险和失败经验

### 7.1 高风险命令组合

以下组合在当前调试设备/固件上出现过“无 PC 数据、控制连接断开或设备重启”：

```text
SYSTem:IF:REMote:MODE SHORT
TRACe:IQ:DATA:REPort FREE
INITiate
```

不要在新软件中默认执行这三个命令的组合。

### 7.2 曾经出现过的失败序列

以下序列虽然被设备接受，且错误查询可能返回正常，但最终没有输出数据，并观察到设备重启风险：

```text
TRACe:UDP:TAG "192.168.50.20",5030,IF
TRACe:UDP:FLAG "192.168.50.20",5030,"OPTIONAL_HEADER"
TRACe:PATH UDP
TRACe:RECord:SELectivity REPort
SYSTem:IF:REMote:MODE SHORT
TRACe:IQ:DATA:NUMBer 256
TRACe:IQ:DATA:REPort MANual
```

不要把该序列作为无条件的默认流程。

### 7.3 设备异常时的处理

如果出现以下现象：

- VISA 控制连接突然断开；
- 设备前面板重启；
- PC 端一直收到 0 字节；
- `SYSTem:ERRor?` 无法返回；
- 设备对后续命令不响应；

应立即：

1. 停止 PC 端等待；
2. 关闭 TCP/UDP 接收器；
3. 不要继续重复发送启动命令；
4. 记录最后发送的完整命令序列；
5. 记录设备固件版本和前面板状态；
6. 重新上电或恢复设备前，先确认现场操作流程。

## 8. 建议提供给新软件的接口

建议同事在他们的软件中抽象出类似以下接口：

```cpp
struct CaptureConfig
{
    QString visaResource;
    double centerFrequencyHz;
    double frequencySpanHz;
    QString protocol;          // TCP 或 UDP
    QString pcIpAddress;
    quint16 pcPort;
    int targetIqPointCount;
    int timeoutMs;
    QString outputFilePath;
};

struct CaptureResult
{
    bool success;
    QString message;
    qint64 bytesReceived;
    qint64 iqSampleCount;
    QString outputFilePath;
    QStringList logs;
};

CaptureResult capture3943b(const CaptureConfig &config);
```

函数内部建议分为四层：

```text
1. VisaSession
   负责打开、写命令、读响应、关闭

2. ScpiCommandBuilder
   负责根据配置生成命令

3. IqReceiver
   负责 TCP/UDP 监听和原始文件保存

4. IqRawParser
   负责数据包校验、IQ 点统计和格式转换
```

不要把 VISA、socket、文件保存和 UI 逻辑全部写在按钮点击函数中，否则后续很难处理超时、停止、重试和设备异常。

## 9. 最小可行实现顺序

给同事的推荐开发顺序：

### 第一步：只读查询工具

实现 VISA 连接和以下命令：

```text
*IDN?
SYSTem:ERRor?
TRACe:PATH?
TRACe:UDP?
TRACe:TCP?
```

### 第二步：PC UDP 接收器

不连接设备，先用模拟 UDP 数据测试：

- 端口监听；
- 文件保存；
- 超时；
- 停止；
- 文件完整性。

### 第三步：路径配置

只写入：

```text
TRACe:UDP:DEL ALL
TRACe:TCP:DEL ALL
TRACe:UDP:TAG ...
TRACe:UDP:FLAG ...
TRACe:PATH UDP
```

确认设备稳定。

### 第四步：小点数 IQ 采集

建议先使用：

```text
IQ 点数：256 或 4096
协议：UDP
```

确认 PC 能收到有效标签为 `901` 的数据包。

### 第五步：加入文件解析和业务处理

确认原始采集稳定后，再增加：

- IQ 点转换；
- CAP 或其他格式输出；
- 连续采集；
- 任务队列；
- UI 进度显示；
- 自动重试。

## 10. 本项目中的参考实现

以下文件可以作为实现参考，但不要直接假设新项目可以无修改复制：

```text
src/services/CeyearVisaService.cpp
```

参考 VISA 会话、SCPI 写入、停止流程和错误处理。

```text
src/services/CeyearVisaQuery.cpp
```

参考默认 VISA Resource、命令序列和风险确认逻辑。

```text
src/services/CeyearIqReceiver.cpp
```

参考 TCP/UDP 接收、超时、目标点数和文件提交逻辑。

```text
src/services/CeyearIfIqPacketParser.cpp
```

参考 IF 数据包识别、IQ 点解析和 IQRAW 校验。

```text
src/tools/ceyear_capture_smoke.cpp
```

参考如何把采集流程封装成独立命令行工具，并提供 dry-run。

```text
docs/3943b-if-iq-bringup-checklist.md
```

参考现场调试顺序和风险检查清单。

## 11. 交接结论

实现 3943B 采集时，最重要的结论是：

1. VISA 只负责控制，IQ 数据通过独立 TCP/UDP 网络通道接收；
2. PC 接收器必须先启动，再发送设备采集命令；
3. 采集成功不能只看 SCPI 错误码，必须验证实际 IQ 字节和有效 IF 数据包；
4. 当前可用的数据路径是网络 IQ/IF Report，不是仪器内部 IQ Record；
5. `0x000EB200`、标签 `901` 和小端 `int16 I/Q` 是解析 IQRAW 的关键；
6. `SYSTem:IF:REMote:MODE SHORT`、`FREE`、`INITiate` 组合存在设备风险；
7. 新软件应先完成只读查询、PC 接收器和小点数采集，再扩展到完整业务流程。

