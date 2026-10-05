#
# Copyright (C) 2022 Xiaomi Corporation
#
# Licensed under the Apache License, Version 2.0 (the "License"); you may not
# use this file except in compliance with the License. You may obtain a copy of
# the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
# WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
# License for the specific language governing permissions and limitations under
# the License.
#

import sys
import yaml
import argparse
from tabulate import tabulate
import re

def parse_config_h(file_path):
    configs = {}
    try:
        with open(file_path, 'r') as file:
            for line in file:
                line = line.strip()
                if line.startswith('#define'):
                    parts = line.split(maxsplit=2)
                    if len(parts) >= 3:
                        key = parts[1].replace('CONFIG_', '')
                        value = parts[2]
                        # Check if the value is a number
                        if re.match(r'^-?\d+$', value):
                            configs[key] = int(value)
                        else:
                            # Check if the value is surrounded by quotes
                            if not (value.startswith('"') and value.endswith('"')):
                                value = f'"{value}"'
                            configs[key] = value.strip('"')
    except FileNotFoundError:
        print("\033[91mFailed to open config.h file. Please compile to generate config.h.\033[0m")
        exit(-1)

    return configs

def parse_yaml(file_path):
    configs = {}
    try:
        with open(file_path, 'r') as file:
            configs = yaml.safe_load(file)
    except FileNotFoundError:
        print("\033[91mFailed to open Reference file. Please provide a valid Reference file.\033[0m")
        exit(-1)
    return configs

def filter_yml_configs(yml_configs, groups):
    group_configs = {}
    for group in groups:
        if group in yml_configs:
            group_configs[group] = yml_configs[group]
    return group_configs

def compare_configs(config_h_configs, group_configs):
    differences = {}
    # Check for configurations present in YAML but missing in config.h
    for group, configs in group_configs.items():
        for key, value in configs.items():
            if value == 0 and key not in config_h_configs:
                continue  # Ignore configurations with value 0 that are missing in config.h
            elif key not in config_h_configs:
                differences[key] = {'config_h_value': 'N/A', 'yml_value': value, 'group': group, 'status': 'Missing in config.h'}

    # Check for configurations with different values between config.h and YAML
    for group, configs in group_configs.items():
        for key, value in configs.items():
            if key in config_h_configs and str(config_h_configs[key]) != str(value):
                differences[key] = {'config_h_value': config_h_configs[key], 'yml_value': value, 'group': group, 'status': 'Mismatch'}
    return differences

def compare_and_print_differences(config_h_configs, group_configs):
    differences = compare_configs(config_h_configs, group_configs)
    print("\n\033[93mDifferences between config.h and Reference:")
    if differences:
        print_differences(differences)
    else:
        print("No differences found.")

def print_differences(differences):
    headers = ["Group", "Config", "config.h", "Reference", "Status"]
    rows = []
    for key, values in differences.items():
        row = [values['group'], key, values['config_h_value'], values['yml_value'], values['status']]
        rows.append(row)
    print(tabulate(rows, headers=headers))

def print_grouped_configs(group_configs):
    print("\n\033[92mConfigs from Reference file grouped by selected groups:")
    headers = ["Group", "Config", "Value"]
    rows = []
    for group, configs in group_configs.items():
        for key, value in configs.items():
            rows.append([group, key, value])
    print(tabulate(rows, headers=headers))

def print_help():
    print("Usage: python config_checker.py")
    print("Options:")
    print_args()

def main():
    parser = argparse.ArgumentParser(description='Check differences between config.h and Reference config files.')
    parser.add_argument('-c', '--config', help='Path to the config.h file', required=True)
    parser.add_argument('-g', '--groups', help='Groups to include, separated by comma', required=True)
    parser.add_argument('-r', '--refs', help='Path to the Reference reference file', required=True)
    parser.add_argument('-d', '--debug', action='store_true', help='Enable debug mode')
    args = parser.parse_args()

    file_path = args.config
    groups = args.groups.split(',')
    refs_path = args.refs

    print("User provided arguments:")
    print(f"Config.h file path: {file_path}")
    print(f"Groups to include: {', '.join(groups)}")
    print(f"Refs file path: {refs_path}")

    config_h_configs = parse_config_h(file_path)
    yml_configs = parse_yaml(refs_path)
    selected_group_configs = filter_yml_configs(yml_configs, groups)

    if args.debug:
        print("config_h_configs:")
        for key, value in config_h_configs.items():
            print(f"{key}: {value}")
        print()

        print("yml_configs:")
        for key, value in yml_configs.items():
            print(f"{key}:")
            for k, v in value.items():
                print(f"  {k}: {v}")
        print()

    print_grouped_configs(selected_group_configs)

    compare_and_print_differences(config_h_configs, selected_group_configs)

if __name__ == "__main__":
    main()
