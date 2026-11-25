#!/usr/bin/env python3
"""
Script to replace queue_data patterns with macros in test_ll_queue.c
"""

import re

with open('test_ll_queue.c', 'r') as f:
    content = f.read()

# Pattern 1: test_data_t* var = (test_data_t*)queue_data_new(sizeof(test_data_t));
# Replace with: Q_NEW(var);
content = re.sub(
    r'test_data_t\* (\w+) = \(test_data_t\*\)queue_data_new\(sizeof\(test_data_t\)\);',
    r'Q_NEW(\1);',
    content
)

# Pattern 2: test_data_t* var = (test_data_t*)queue_data_new_from_pool(pool);
# Replace with: Q_NEW_POOL(var, pool);
content = re.sub(
    r'test_data_t\* (\w+) = \(test_data_t\*\)queue_data_new_from_pool\(([^)]+)\);',
    r'Q_NEW_POOL(\1, \2);',
    content
)

# Pattern 3: test_data_t* var = (test_data_t*)queue_data_get(q);
# Replace with: Q_GET(q, var);
content = re.sub(
    r'test_data_t\* (\w+) = \(test_data_t\*\)queue_data_get\(([^)]+)\);',
    r'Q_GET(\2, \1);',
    content
)

# Pattern 4: queue_data_put(q, var, var->id)
# Replace with: Q_PUT(q, var)
content = re.sub(
    r'queue_data_put\(([^,]+), ([^,]+), \2->id\)',
    r'Q_PUT(\1, \2)',
    content
)

# Pattern 5: queue_data_put_first(q, var, var->id)
# Replace with: Q_PUT_FIRST(q, var)
content = re.sub(
    r'queue_data_put_first\(([^,]+), ([^,]+), \2->id\)',
    r'Q_PUT_FIRST(\1, \2)',
    content
)

# Pattern 6: queue_data_free(var);
# Replace with: Q_FREE(var);
content = re.sub(
    r'queue_data_free\((\w+)\);',
    r'Q_FREE(\1);',
    content
)

# Pattern 7: test_data_t* var = (test_data_t*)queue_find_data_by_id(q, id);
# Replace with: Q_FIND(q, id, var);
content = re.sub(
    r'test_data_t\* (\w+) = \(test_data_t\*\)queue_find_data_by_id\(([^,]+), ([^)]+)\);',
    r'Q_FIND(\2, \3, \1);',
    content
)

# Pattern 8: queue_remove_data(q, var)
# Replace with: Q_REMOVE(q, var)
content = re.sub(
    r'queue_remove_data\(([^,]+), ([^)]+)\)',
    r'Q_REMOVE(\1, \2)',
    content
)

with open('test_ll_queue.c', 'w') as f:
    f.write(content)

print("All patterns replaced successfully!")