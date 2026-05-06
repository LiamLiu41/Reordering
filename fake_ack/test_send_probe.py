#!/usr/bin/env python3
# -*- coding: utf-8 -*-

# from scapy.all import rdpcap, sendp, wrpcap
from scapy.all import *

def replay_pcap(pcap_file):
    # 读取 pcap 文件
    print(f"replaying: {pcap_file}")
    packets = rdpcap(pcap_file)

    # 取第一个包
    pkt = packets[0]

    # 循环发送指定次数
    count = 10  # 修改为你想要的次数
    for _ in range(count):
        sendp(pkt, iface="eth6", verbose=False)  # 修改为你的网卡名

def modify_fields_and_save(
    pcap_file,
    ack_req=None,
    status=None,
    class_spec=None,
    add_status=None, 
    dqpn=None,
    rsvd8_2=None,
    output_file="modified.pcap"
):
    packets = rdpcap(pcap_file)
    print(f"总包数: {len(packets)}")
    if len(packets) == 0:
        print("没有包！")
        return

    pkt = packets[0]
    raw_layer = pkt.getlayer("Raw")
    if raw_layer is None:
        print("没有Raw层！")
        return

    raw_data = raw_layer.load
    print(f"原始 load (16进制): {raw_data.hex()}")

    mod_data = bytearray(raw_data)

    # dqpn: 5-7字节（3字节）
    if dqpn is not None:
        mod_data[5:8] = dqpn.to_bytes(3, byteorder='big')
    # ack_req: 第9字节最高位
    if ack_req is not None:
        if ack_req:
            mod_data[8] |= 0x80  # 置位
        else:
            mod_data[8] &= 0x7F  # 清零
    # rsvd8_2: 16字节（1字节）
    if rsvd8_2 is not None:
        mod_data[16] = rsvd8_2
    # status: 24-25字节
    if status is not None:
        mod_data[24:26] = status.to_bytes(2, byteorder='big')
    # class_spec: 26-27字节
    if class_spec is not None:
        mod_data[26:28] = class_spec.to_bytes(2, byteorder='big')
    # add_status: 38-39字节
    if add_status is not None:
        mod_data[38:40] = add_status.to_bytes(2, byteorder='big')

    print(f"修改后 load (16进制): {mod_data.hex()}")

    raw_layer.load = bytes(mod_data)
    wrpcap(output_file, [pkt])
    print(f"已保存到 {output_file}")

def parse_raw_payload(raw_data):
    # bth
    opcode = raw_data[0]
    raw_byte2 = raw_data[1]
    se = (raw_byte2 >> 7) & 0b1
    mr = (raw_byte2 >> 6) & 0b1
    pad_count = (raw_byte2 >> 4) & 0b11
    hdr_ver = (raw_byte2) & 0b00001111
    pkey = int.from_bytes(raw_data[2:4], byteorder='big')
    rsvd8 = raw_data[4]
    dqpn = int.from_bytes(raw_data[5:8], byteorder='big')
    raw_byte9 = raw_data[8]
    ack_req = (raw_byte9 >> 7) & 0b1
    rsvd7 = (raw_byte9) & 0b01111111
    psn = int.from_bytes(raw_data[9:12], byteorder='big')
    # deth
    q_key = int.from_bytes(raw_data[12:16], byteorder='big')
    rsvd8_2 = raw_data[16]
    sqpn = int.from_bytes(raw_data[17:20], byteorder='big')
    # mad
    base_ver = raw_data[20]
    mgmt_class = raw_data[21]
    class_ver = raw_data[22]
    r_method = raw_data[23]
    status = int.from_bytes(raw_data[24:26], byteorder='big')
    class_spec = int.from_bytes(raw_data[26:28], byteorder='big')
    tid_high = int.from_bytes(raw_data[28:32], byteorder='big')
    tid_low = int.from_bytes(raw_data[32:36], byteorder='big')
    attr_id = int.from_bytes(raw_data[36:38], byteorder='big')
    add_status = int.from_bytes(raw_data[38:40], byteorder='big')
    attr_mod = int.from_bytes(raw_data[40:44], byteorder='big')
    np_key_high = int.from_bytes(raw_data[44:48], byteorder='big')
    np_key_low = int.from_bytes(raw_data[48:52], byteorder='big')
    cap_mask = int.from_bytes(raw_data[52:56], byteorder='big')
    req_send_ts = int.from_bytes(raw_data[56:60], byteorder='big')
    req_recv_ts = int.from_bytes(raw_data[60:64], byteorder='big')
    rsp_send_ts = int.from_bytes(raw_data[64:68], byteorder='big')
    print(f"  [BTH]")
    print(f"    opcode: {opcode}")
    print(f"    se: {se}")
    print(f"    mr: {mr}")
    print(f"    pad_count: {pad_count}")
    print(f"    hdr_ver: {hdr_ver}")
    print(f"    pkey: {pkey}")
    print(f"    rsvd8: {rsvd8}")
    print(f"    dqpn: {dqpn}")
    print(f"    ack_req: {ack_req}")
    print(f"    rsvd7: {rsvd7}")
    print(f"    psn: {psn}")
    print(f"  [DETH]")
    print(f"    q_key: {q_key}")
    print(f"    rsvd8_2: {rsvd8_2}")
    print(f"    sqpn: {sqpn}")
    print(f"  [MAD]")
    print(f"    base_ver: {base_ver}")
    print(f"    mgmt_class: {mgmt_class}")
    print(f"    class_ver: {class_ver}")
    print(f"    r_method: {r_method}")
    print(f"    status: {status}")
    print(f"    class_spec: {class_spec}")
    print(f"    tid_high: {tid_high}")
    print(f"    tid_low: {tid_low}")
    print(f"    attr_id: {attr_id}")
    print(f"    add_status: {add_status}")
    print(f"    attr_mod: {attr_mod}")
    print(f"    np_key_high: {np_key_high}")
    print(f"    np_key_low: {np_key_low}")
    print(f"    cap_mask: {cap_mask}")
    print(f"    req_send_ts: {req_send_ts}")
    print(f"    req_recv_ts: {req_recv_ts}")
    print(f"    rsp_send_ts: {rsp_send_ts}")


def parse_pcap_fields(pcap_file):
    packets = rdpcap(pcap_file)
    print(f"总包数: {len(packets)}")
    for idx, pkt in enumerate(packets):
        print("="*30)
        print(f"包序号: {idx+1}")
        for layer in pkt.layers():
            l = pkt.getlayer(layer)
            print(f"层: {layer.__name__}")
            if layer.__name__ == "Raw":
                raw_data = l.load
                print(f"  load (内容): {raw_data}")
                print(f"  load (16进制): {raw_data.hex()}")
                parse_raw_payload(raw_data)
            else:
                for field in l.fields_desc:
                    field_name = field.name
                    value = getattr(l, field_name, None)
                    print(f"  {field_name}: {value}")
                raw_bytes = bytes(l)
                hex_str = raw_bytes.hex()
        print("="*30)

def verify_and_generate_ack(input_pcap, output_pcap):
    # 1. 加载模板
    packets = rdpcap(input_pcap)
    if not packets:
        print("错误：无法读取 pcap 文件")
        return
    
    pkt = packets[0]
    if not pkt.haslayer(Raw):
        print("错误：该报文没有 Raw 层，无法修改内容")
        return

    # 获取原始字节
    raw_data = bytearray(pkt[Raw].load)
    print(f"原始报文长度: {len(raw_data)} 字节")

    # --- 关键修改点：将报文转化为 ACK 格式 ---
    
    # 修改 BTH Opcode: 偏移量 0
    # RoCE v2 RC Acknowledge 的 Opcode 通常是 0x11
    raw_data[0] = 0x11
    
    # 修改 BTH PSN: 偏移量 9-11 (假设我们要设为 100)
    target_psn = 100
    raw_data[9:12] = target_psn.to_bytes(3, byteorder='big')
    
    # 修改 AETH Syndrome: 偏移量 12 (BTH 之后的第一字节)
    # 0x06 代表正向确认 (ACK)，高 3 位为 000 是 ACK，011 是 NACK
    # 二进制 00000110 -> 0x06
    if len(raw_data) > 12:
        raw_data[12] = 0x06 

    # --- 保存修改 ---
    pkt[Raw].load = bytes(raw_data)
    
    # 移除 UDP 和 IP 的校验和，让 Scapy 在保存/发送时重新计算（如果是标准的 UDP/IP）
    if 'UDP' in pkt: del pkt['UDP'].chksum
    if 'IP' in pkt: del pkt['IP'].chksum

    wrpcap(output_pcap, [pkt])
    print(f"验证报文已生成: {output_pcap}")
    print(f"预期修改：Opcode=0x11, PSN={target_psn}")

from scapy.all import rdpcap, wrpcap, Raw

def generate_nack_test(input_pcap, output_pcap, target_psn=85):
    packets = rdpcap(input_pcap)
    pkt = packets[0]
    raw_data = bytearray(pkt[Raw].load)

    # 1. Opcode 保持 0x11 (ACK/NACK)
    raw_data[0] = 0x11
    
    # 2. 设置目标 PSN (比如 85)
    raw_data[9:12] = target_psn.to_bytes(3, byteorder='big')
    
    # 3. 设置 AETH Syndrome 为 NACK - Sequence Error
    # 0x60 = 01100000 (高三位 011 代表 NACK)
    if len(raw_data) > 12:
        raw_data[12] = 0x60
        # 备注：有些场景需要 MSN (Message Sequence Number) 连续，
        # 如果重传不触发，可能需要微调 13-15 字节。

    pkt[Raw].load = bytes(raw_data)
    wrpcap(output_pcap, [pkt])
    print(f"NACK 验证报文已生成: {output_pcap}, PSN={target_psn}")

if __name__ == "__main__":
    generate_nack_test("bf3_test_pcap.pcap", "verify_nack_step2.pcap", target_psn=85)

# if __name__ == "__main__":
#     # 使用你的原始 pcap 文件名
#     verify_and_generate_ack("bf3_test_pcap.pcap", "verify_ack_step1.pcap")

# if __name__ == "__main__":
#     # replay_pcap("bf3_test_pcap.pcap")
#     # parse_pcap_fields("bf3_test_pcap.pcap")
#     # modify_fields_and_save("bf3_test_pcap.pcap", status=0x1, dqpn=0x1, output_file="status_1.pcap")
#     # modify_fields_and_save("bf3_test_pcap.pcap", status=0x80, dqpn=0x1, output_file="status_0x80.pcap")
#     # modify_fields_and_save("bf3_test_pcap.pcap", status=0x40, dqpn=0x1, output_file="status_0x40.pcap")
#     # modify_fields_and_save("bf3_test_pcap.pcap", status=0x20, dqpn=0x1, output_file="status_0x20.pcap")
#     # modify_fields_and_save("bf3_test_pcap.pcap", class_spec=0x1, dqpn=0x1, output_file="class_spec_1.pcap")
#     # modify_fields_and_save("bf3_test_pcap.pcap", add_status=0x1, dqpn=0x1, output_file="add_status_1.pcap")
#     # modify_fields_and_save("bf3_test_pcap.pcap", status=0x1, output_file="status_1_qpn10.pcap")
#     # modify_fields_and_save("bf3_test_pcap.pcap", class_spec=0x1, output_file="class_spec_1_qpn10.pcap")
#     # modify_fields_and_save("bf3_test_pcap.pcap", add_status=0x1, output_file="add_status_1_qpn10.pcap")
#     # modify_fields_and_save("bf3_test_pcap.pcap", dqpn=0x1, output_file="dqpn_0x1.pcap")
#     # modify_fields_and_save("bf3_test_pcap.pcap", dqpn=0x1, ack_req=0, output_file="ack_req_0.pcap")
#     # modify_fields_and_save("bf3_test_pcap.pcap", dqpn=0x1, ack_req=0, class_spec=0x1, output_file="ack_req_0_class_spec_1.pcap")
#     # modify_fields_and_save("bf3_test_pcap.pcap", dqpn=0x1, ack_req=0, status=0x1, output_file="ack_req_0_status_1.pcap")
#     # modify_fields_and_save("bf3_test_pcap.pcap", dqpn=0x1, ack_req=0, add_status=0x1, output_file="ack_req_0_add_status_1.pcap")
#     # modify_fields_and_save("bf3_test_pcap.pcap", dqpn=0x1, rsvd8_2=0x1, output_file="rsvd8_2_0x1.pcap")

#     # modify_fields_and_save("bf3_test_pcap.pcap", ack_req=0, output_file="qpn10_ack_req_0.pcap")
#     # modify_fields_and_save("bf3_test_pcap.pcap", ack_req=0, class_spec=0x1, output_file="qpn10_ack_req_0_class_spec_1.pcap")
#     # modify_fields_and_save("bf3_test_pcap.pcap", ack_req=0, status=0x1, output_file="qpn10_ack_req_0_status_1.pcap")
#     # modify_fields_and_save("bf3_test_pcap.pcap", ack_req=0, add_status=0x1, output_file="qpn10_ack_req_0_add_status_1.pcap")

#     # replay_pcap("qpn10_ack_req_0.pcap")
#     # replay_pcap("qpn10_ack_req_0_class_spec_1.pcap")
#     # replay_pcap("qpn10_ack_req_0_status_1.pcap")
#     replay_pcap("qpn10_ack_req_0_add_status_1.pcap")

#     # replay_pcap("dqpn_0x1.pcap")
#     # replay_pcap("ack_req_0.pcap")
#     # replay_pcap("ack_req_0_class_spec_1.pcap")
#     # replay_pcap("ack_req_0_status_1.pcap")
#     # replay_pcap("ack_req_0_add_status_1.pcap")

#     # replay_pcap("status_1_qpn10.pcap")
#     # replay_pcap("class_spec_1_qpn10.pcap")
#     # replay_pcap("add_status_1_qpn10.pcap")
#     # replay_pcap("bf3_test_pcap.pcap")

#     # replay_pcap("status_1.pcap")
#     # replay_pcap("status_0x80.pcap")
#     # replay_pcap("status_0x40.pcap")
#     # replay_pcap("status_0x20.pcap")
#     # replay_pcap("add_status_1.pcap")
#     # replay_pcap("class_spec_1.pcap")
#     # replay_pcap("rsvd8_2_0x1.pcap")
