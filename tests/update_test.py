#!/usr/bin/env python3
"""
Script to update test_ll_queue.c for xxx=0 architecture
Changes all queue_data_* calls to use struct ll_entry* intermediate
"""

import re

# Read the file
with open('test_ll_queue.c', 'r') as f:
    content = f.read()

# Pattern 1: queue_data_new with variable declaration
# FROM: test_data_t* data = (test_data_t*)queue_data_new(sizeof(test_data_t));
# TO: struct ll_entry* entry = (struct ll_entry*)queue_data_new(sizeof(test_data_t));
#     test_data_t* data = (test_data_t*)entry->data;

# Find all occurrences and replace
lines = content.split('\n')
new_lines = []
i = 0
while i < len(lines):
    line = lines[i]
    
    # Pattern 1: test_data_t* data = (test_data_t*)queue_data_new
    match = re.match(r'(\s*)test_data_t\* (\w+) = \(test_data_t\*\)queue_data_new\(sizeof\(test_data_t\)\);', line)
    if match:
        indent = match.group(1)
        var_name = match.group(2)
        # Replace with two lines
        new_lines.append(f'{indent}struct ll_entry* entry_{var_name} = (struct ll_entry*)queue_data_new(sizeof(test_data_t));')
        new_lines.append(f'{indent}test_data_t* {var_name} = (test_data_t*)entry_{var_name}->data;')
        i += 1
        continue
    
    # Pattern 2: test_data_t* var = (test_data_t*)queue_data_get
    match = re.match(r'(\s*)test_data_t\* (\w+) = \(test_data_t\*\)queue_data_get\(([^)]+)\);', line)
    if match:
        indent = match.group(1)
        var_name = match.group(2)
        queue_var = match.group(3)
        # Replace with two lines
        new_lines.append(f'{indent}struct ll_entry* entry_{var_name} = (struct ll_entry*)queue_data_get({queue_var});')
        new_lines.append(f'{indent}test_data_t* {var_name} = entry_{var_name} ? (test_data_t*)entry_{var_name}->data : NULL;')
        i += 1
        continue
    
    # Pattern 3: queue_data_put with data variable
    # Need to find the corresponding entry variable
    match = re.match(r'(\s*)(\w+)\(q, (\w+), (\w+)->id\)', line)
    if match and 'queue_data_put' in line:
        func = match.group(2)
        var = match.group(3)
        # Replace data with entry variable
        new_line = line.replace(f'{var}, {var}->id)', f'entry_{var}, {var}->id)')
        new_lines.append(new_line)
        i += 1
        continue
    
    # Pattern 4: queue_data_free with data variable
    match = re.match(r'(\s*)queue_data_free\((\w+)\);', line)
    if match:
        indent = match.group(1)
        var = match.group(2)
        # Check if this is a data variable (not entry)
        if not var.startswith('entry_') and var != 'data' and var != 'item':
            # Replace with entry variable
            new_lines.append(f'{indent}queue_data_free(entry_{var});')
        else:
            new_lines.append(line)
        i += 1
        continue
    
    new_lines.append(line)
    i += 1

# Write the updated file
with open('test_ll_queue.c', 'w') as f:
    f.write('\n'.join(new_lines))

print("File updated successfully!")