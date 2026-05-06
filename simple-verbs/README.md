# RDMA RC 示例程序

两个 RDMA RC (Reliable Connection) 示例程序：
- `rc_write`: 演示 RDMA Write 操作
- `rc_send`: 演示 RDMA Send/Recv 操作

## 编译

```bash
make
```

单独编译：
```bash
make rc_write
make rc_send
```

## 使用方法

### rc_write

接收端:
```bash
./build/rc_write -r [-d device_name] [-i ib_port] [-p port]
```

发送端:
```bash
./build/rc_write -s -a <receiver_ip> [-d device_name] [-i ib_port] [-p port]
```

### rc_send

接收端:
```bash
./build/rc_send -r [-d device_name] [-i ib_port] [-p port]
```

发送端:
```bash
./build/rc_send -s -a <receiver_ip> [-d device_name] [-i ib_port] [-p port]
```

## 参数说明

- `-s/--sender`: 发送方模式
- `-r/--receiver`: 接收方模式
- `-d/--device`: RDMA 设备名称
- `-i/--ib_port`: IB 端口号（默认: 1）
- `-a/--address`: 接收方 IP 地址
- `-p/--port`: TCP 连接交换端口（默认: 5555）
- `-h/--help`: 显示帮助信息

## 示例

接收端：
```bash
./build/rc_write -r -d mlx5_4 -p 5555
```

发送端：
```bash
./build/rc_write -s -a 100.0.0.16 -d mlx5_4 -p 5555
```