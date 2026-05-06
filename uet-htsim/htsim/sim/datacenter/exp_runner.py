#!/usr/bin/env python3
"""
实验运行器
功能：
1. 按照stage顺序执行实验
2. 每个stage内的实验并行执行
3. 监控和汇报每个实验的执行状态
4. 支持CPU亲和性设置
"""

import json
import subprocess
import sys
import os
import time
from pathlib import Path
from datetime import datetime
from typing import List, Dict, Any, Optional
import threading
import queue


class ExperimentRunner:
    """实验运行器"""
    
    def __init__(self, config_file: str, working_dir: Optional[str] = None):
        """
        初始化实验运行器
        
        Args:
            config_file: JSON配置文件路径
            working_dir: 工作目录（默认为配置文件所在目录的父目录）
        """
        self.config_file = config_file
        self.config = self._load_config()
        
        # 设置工作目录（优先级：命令行参数 > JSON配置 > 默认值）
        if working_dir:
            # 命令行参数优先
            self.working_dir = Path(working_dir)
        elif 'working_dir' in self.config:
            # 从JSON配置读取
            self.working_dir = Path(self.config['working_dir'])
        else:
            # 默认使用配置文件父目录的父目录作为工作目录
            self.working_dir = Path(config_file).parent.parent
        
        self.results = []  # 存储所有实验结果
        
    def _load_config(self) -> Dict[str, Any]:
        """加载JSON配置文件"""
        try:
            with open(self.config_file, 'r', encoding='utf-8') as f:
                return json.load(f)
        except Exception as e:
            print(f"❌ 错误：无法加载配置文件 {self.config_file}: {e}")
            sys.exit(1)
    
    def _build_command(self, base_cmd: str, common_args: Dict, exp_args: Dict,
                      common_positional: List = None, exp_positional: List = None) -> str:
        """
        构建完整的命令行
        
        Args:
            base_cmd: 基础命令（如 ./htsim_uec）
            common_args: 公共参数（键值对）
            exp_args: 实验特定参数（键值对）
            common_positional: 公共位置参数（列表）
            exp_positional: 实验特定位置参数（列表）
            
        Returns:
            完整的命令字符串
        """
        cmd_parts = [base_cmd]
        
        # 处理位置参数（合并公共和实验特定的位置参数）
        all_positional = []
        if common_positional:
            all_positional.extend(common_positional)
        if exp_positional:
            all_positional.extend(exp_positional)
        
        # 添加位置参数（在键值参数之前）
        for pos_arg in all_positional:
            cmd_parts.append(str(pos_arg))
        
        # 合并公共参数和实验参数，实验参数会覆盖公共参数
        all_args = {**common_args, **exp_args}
        
        # 添加键值参数
        for key, value in all_args.items():
            if value is None:  # 用于处理标志参数（如 -sender_cc_only）
                cmd_parts.append(key)
            elif isinstance(value, list):  # 处理列表参数（如 -ecn [100, 200]）
                cmd_parts.append(f"{key} {' '.join(map(str, value))}")
            else:
                cmd_parts.append(f"{key} {value}")
        
        return " ".join(cmd_parts)
    
    def _create_output_dirs(self, stdout_path: str, output_path: Optional[str] = None):
        """创建输出目录"""
        # 为stdout创建目录
        stdout_dir = Path(stdout_path).parent
        stdout_dir.mkdir(parents=True, exist_ok=True)
        
        # 为输出文件创建目录（如果有-o参数）
        if output_path:
            output_dir = Path(output_path).parent
            output_dir.mkdir(parents=True, exist_ok=True)
    
    def _run_experiment(self, exp_name: str, command: str, stdout_path: str, 
                       cpu_id: Optional[int] = None) -> Dict[str, Any]:
        """
        运行单个实验
        
        Args:
            exp_name: 实验名称
            command: 要执行的命令
            stdout_path: 标准输出重定向路径
            cpu_id: CPU ID（用于CPU亲和性设置）
            
        Returns:
            包含实验结果的字典
        """
        start_time = time.time()
        result = {
            'name': exp_name,
            'command': command,
            'stdout': stdout_path,
            'start_time': datetime.now().strftime('%Y-%m-%d %H:%M:%S'),
            'status': 'unknown',
            'return_code': None,
            'duration': 0,
            'cpu_id': cpu_id
        }
        
        try:
            print(f"🚀 启动实验: {exp_name}")
            if cpu_id is not None:
                print(f"   CPU: {cpu_id}")
            print(f"   命令: {command}")
            print(f"   输出: {stdout_path}")
            
            # 创建输出目录
            self._create_output_dirs(stdout_path)
            
            # 打开输出文件
            with open(stdout_path, 'w') as stdout_file:
                # 构建进程启动参数
                process_args = {
                    'shell': True,
                    'stdout': stdout_file,
                    'stderr': subprocess.STDOUT,
                    'cwd': str(self.working_dir)
                }
                
                # 如果指定了CPU ID，使用taskset设置CPU亲和性
                if cpu_id is not None:
                    command = f"taskset -c {cpu_id} {command}"
                
                # 启动进程
                process = subprocess.Popen(command, **process_args)
                
                # 等待进程完成
                return_code = process.wait()
                
                # 记录结果
                duration = time.time() - start_time
                result['return_code'] = return_code
                result['duration'] = duration
                
                if return_code == 0:
                    result['status'] = 'success'
                    print(f"✅ 实验完成: {exp_name} (耗时: {duration:.2f}秒)")
                else:
                    result['status'] = 'failed'
                    print(f"❌ 实验失败: {exp_name} (返回码: {return_code}, 耗时: {duration:.2f}秒)")
                
        except Exception as e:
            duration = time.time() - start_time
            result['status'] = 'error'
            result['error'] = str(e)
            result['duration'] = duration
            print(f"💥 实验异常: {exp_name} - {e}")
        
        return result
    
    def _run_stage(self, stage: Dict[str, Any], cpu_offset: int = 0) -> List[Dict[str, Any]]:
        """
        运行单个stage的所有实验（支持并行或顺序执行）
        
        Args:
            stage: stage配置
            cpu_offset: CPU偏移量（用于分配不同的CPU）
            
        Returns:
            该stage所有实验的结果列表
        """
        stage_name = stage.get('name', 'unnamed_stage')
        description = stage.get('description', '')
        base_cmd = stage['base_command']
        common_args = stage.get('common_args', {})
        common_positional = stage.get('common_positional_args', [])
        experiments = stage['experiments']
        # 默认并行执行，可以通过 "parallel": false 设置为顺序执行
        is_parallel = stage.get('parallel', True)
        
        print(f"\n{'='*80}")
        print(f"📊 Stage: {stage_name}")
        if description:
            print(f"   描述: {description}")
        print(f"   实验数量: {len(experiments)}")
        print(f"   执行模式: {'并行 🚀' if is_parallel else '顺序 ⏭️'}")
        print(f"{'='*80}\n")
        
        stage_results = []
        
        if is_parallel:
            # 并行执行模式
            threads = []
            result_queue = queue.Queue()
            
            # 启动所有实验线程
            for idx, exp in enumerate(experiments):
                exp_name = exp['name']
                exp_args = exp.get('args', {})
                exp_positional = exp.get('positional_args', [])
                stdout_path = exp['stdout']
                
                # 分配CPU（如果系统支持）
                cpu_id = cpu_offset + idx
                
                # 构建完整命令
                command = self._build_command(base_cmd, common_args, exp_args,
                                             common_positional, exp_positional)
                
                # 创建并启动线程
                thread = threading.Thread(
                    target=lambda: result_queue.put(
                        self._run_experiment(exp_name, command, stdout_path, cpu_id)
                    )
                )
                thread.start()
                threads.append(thread)
                
                # 稍微延迟，避免同时启动过多进程
                time.sleep(0.1)
            
            # 等待所有线程完成
            for thread in threads:
                thread.join()
            
            # 收集结果
            while not result_queue.empty():
                stage_results.append(result_queue.get())
        else:
            # 顺序执行模式
            for idx, exp in enumerate(experiments):
                exp_name = exp['name']
                exp_args = exp.get('args', {})
                exp_positional = exp.get('positional_args', [])
                stdout_path = exp['stdout']
                
                # 分配CPU
                cpu_id = cpu_offset + idx
                
                # 构建完整命令
                command = self._build_command(base_cmd, common_args, exp_args,
                                             common_positional, exp_positional)
                
                # 顺序执行实验
                result = self._run_experiment(exp_name, command, stdout_path, cpu_id)
                stage_results.append(result)
                
                # 如果实验失败，打印警告但继续执行
                if result['status'] != 'success':
                    print(f"⚠️  实验 {exp_name} 未成功完成，继续执行下一个实验...\n")
        
        return stage_results
    
    def run_all_stages(self, start_cpu: int = 0, stage_names: Optional[List[str]] = None):
        """
        运行所有stage（或指定的stage）
        
        Args:
            start_cpu: 起始CPU ID
            stage_names: 要执行的stage名称列表，None表示执行所有stage
        """
        all_stages = self.config.get('stages', [])
        
        # 如果指定了stage名称，则过滤出对应的stage
        if stage_names:
            stages = []
            stage_name_set = set(stage_names)
            available_names = [s.get('name', 'unnamed') for s in all_stages]
            
            # 检查是否有无效的stage名称
            invalid_names = stage_name_set - set(available_names)
            if invalid_names:
                print(f"❌ 错误：以下stage名称不存在: {', '.join(invalid_names)}")
                print(f"   可用的stage名称: {', '.join(available_names)}")
                return
            
            # 按照配置文件中的顺序保留指定的stage
            for stage in all_stages:
                if stage.get('name', 'unnamed') in stage_name_set:
                    stages.append(stage)
        else:
            stages = all_stages
        
        print(f"\n{'='*80}")
        print(f"🎯 实验运行器启动")
        print(f"   配置文件: {self.config_file}")
        print(f"   工作目录: {self.working_dir}")
        if stage_names:
            print(f"   指定Stages: {', '.join(stage_names)}")
            print(f"   执行Stages数量: {len(stages)}/{len(all_stages)}")
        else:
            print(f"   Stages数量: {len(stages)}")
        print(f"{'='*80}\n")
        
        overall_start = time.time()
        
        for stage_idx, stage in enumerate(stages):
            stage_start = time.time()
            
            # 运行该stage
            stage_results = self._run_stage(stage, cpu_offset=start_cpu)
            self.results.extend(stage_results)
            
            # 统计该stage的结果
            success_count = sum(1 for r in stage_results if r['status'] == 'success')
            failed_count = sum(1 for r in stage_results if r['status'] == 'failed')
            error_count = sum(1 for r in stage_results if r['status'] == 'error')
            stage_duration = time.time() - stage_start
            
            print(f"\n{'='*80}")
            print(f"📈 Stage {stage_idx + 1}/{len(stages)} 完成: {stage['name']}")
            print(f"   成功: {success_count}, 失败: {failed_count}, 异常: {error_count}")
            print(f"   耗时: {stage_duration:.2f}秒")
            print(f"{'='*80}\n")
            
            # 如果有失败或异常，询问是否继续
            if failed_count > 0 or error_count > 0:
                print(f"⚠️  警告: Stage {stage['name']} 中有 {failed_count + error_count} 个实验未成功完成")
        
        overall_duration = time.time() - overall_start
        self._print_summary(overall_duration)
    
    def _print_summary(self, total_duration: float):
        """打印总结报告"""
        print(f"\n{'='*80}")
        print(f"📋 实验运行总结")
        print(f"{'='*80}")
        print(f"总实验数: {len(self.results)}")
        
        success_count = sum(1 for r in self.results if r['status'] == 'success')
        failed_count = sum(1 for r in self.results if r['status'] == 'failed')
        error_count = sum(1 for r in self.results if r['status'] == 'error')
        
        print(f"成功: {success_count}")
        print(f"失败: {failed_count}")
        print(f"异常: {error_count}")
        print(f"总耗时: {total_duration:.2f}秒")
        print(f"{'='*80}\n")
        
        # 打印详细结果
        print("详细结果:")
        print(f"{'实验名称':<30} {'状态':<10} {'耗时(秒)':<12} {'返回码':<10}")
        print("-" * 80)
        for result in self.results:
            status_symbol = {
                'success': '✅',
                'failed': '❌',
                'error': '💥'
            }.get(result['status'], '❓')
            
            print(f"{result['name']:<30} {status_symbol} {result['status']:<8} "
                  f"{result['duration']:>10.2f}  {str(result['return_code']):<10}")
        
        print(f"{'='*80}\n")
    
    def save_results(self, output_file: str):
        """保存结果到JSON文件"""
        try:
            with open(output_file, 'w', encoding='utf-8') as f:
                json.dump(self.results, f, indent=2, ensure_ascii=False)
            print(f"✅ 结果已保存到: {output_file}")
        except Exception as e:
            print(f"❌ 保存结果失败: {e}")


def main():
    """主函数"""
    import argparse
    
    parser = argparse.ArgumentParser(
        description='实验运行器 - 按照JSON配置并行运行多个实验',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
示例:
  # 使用默认配置文件运行
  python exp_runner.py
  
  # 指定配置文件
  python exp_runner.py -c my_config.json
  
  # 指定工作目录和起始CPU
  python exp_runner.py -c config.json -w /path/to/work -cpu 0
  
  # 保存结果到文件
  python exp_runner.py -c config.json -o results.json
        """
    )
    
    parser.add_argument('-c', '--config', 
                       default='exp_config/template.json',
                       help='JSON配置文件路径 (默认: exp_config/template.json)')
    parser.add_argument('-w', '--workdir',
                       default=None,
                       help='工作目录 (默认: 配置文件所在目录的父目录)')
    parser.add_argument('--cpu', type=int, default=0,
                       help='起始CPU ID (默认: 0)')
    parser.add_argument('-o', '--output',
                       default=None,
                       help='保存结果的JSON文件路径')
    parser.add_argument('-s', '--stages',
                       nargs='+',
                       default=None,
                       help='要执行的stage名称列表（默认执行所有stage）')
    parser.add_argument('--list-stages',
                       action='store_true',
                       help='列出配置文件中所有可用的stage名称')
    
    args = parser.parse_args()
    
    # 创建运行器
    runner = ExperimentRunner(args.config, args.workdir)
    
    # 如果只是列出stage名称
    if args.list_stages:
        stages = runner.config.get('stages', [])
        print(f"\n配置文件 {args.config} 中的stage列表:")
        print("-" * 50)
        for idx, stage in enumerate(stages, 1):
            name = stage.get('name', 'unnamed')
            desc = stage.get('description', '')
            exp_count = len(stage.get('experiments', []))
            print(f"  {idx}. {name} ({exp_count} experiments)")
            if desc:
                print(f"     描述: {desc}")
        print("-" * 50)
        sys.exit(0)
    
    # 运行所有实验
    try:
        runner.run_all_stages(start_cpu=args.cpu, stage_names=args.stages)
    except KeyboardInterrupt:
        print("\n\n⚠️  用户中断实验运行")
        sys.exit(1)
    
    # 保存结果（如果指定）
    if args.output:
        runner.save_results(args.output)
    
    # 根据结果返回退出码
    failed_count = sum(1 for r in runner.results if r['status'] != 'success')
    sys.exit(0 if failed_count == 0 else 1)


if __name__ == '__main__':
    main()

