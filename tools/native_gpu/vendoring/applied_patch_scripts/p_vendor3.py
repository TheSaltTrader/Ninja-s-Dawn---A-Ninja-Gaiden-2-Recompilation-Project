p = r"C:/Users/renoi/.claude/jobs/6397a53c/tmp/vendor_rtc_d3d12.py"
s = open(p, encoding="utf-8").read()
add = '''    "include/rex/graphics/d3d12/command_processor.h": "command_processor.h",
    "src/graphics/d3d12/command_processor.cpp": "command_processor.cpp",
    "include/rex/graphics/d3d12/pipeline_cache.h": "pipeline_cache.h",
    "src/graphics/d3d12/pipeline_cache.cpp": "pipeline_cache.cpp",
    "include/rex/graphics/d3d12/primitive_processor.h": "primitive_processor.h",
    "src/graphics/d3d12/primitive_processor.cpp": "primitive_processor.cpp",
    "include/rex/graphics/primitive_processor.h": "primitive_processor_base.h",
    "src/graphics/primitive_processor.cpp": "primitive_processor_base.cpp",
    "include/rex/graphics/d3d12/shader.h": "shader.h",
    "src/graphics/d3d12/shader.cpp": "shader.cpp",
    "include/rex/ui/d3d12/d3d12_descriptor_heap_pool.h": "d3d12_descriptor_heap_pool.h",
    "src/ui/d3d12/d3d12_descriptor_heap_pool.cpp": "d3d12_descriptor_heap_pool.cpp",
}'''
if '"src/graphics/d3d12/command_processor.cpp"' not in s:
    s = s.replace('    "src/graphics/d3d12/texture_cache.cpp": "texture_cache.cpp",\n}', '    "src/graphics/d3d12/texture_cache.cpp": "texture_cache.cpp",\n' + add)
    s = s.replace('''    "<rex/graphics/d3d12/command_processor.h>": '"rtc_d3d12/facade.h"',''', '''    "<rex/graphics/d3d12/command_processor.h>": '"rtc_d3d12/command_processor.h"',
    "<rex/graphics/command_processor.h>": '"rtc_d3d12/cp_base.h"',
    "<rex/graphics/d3d12/graphics_system.h>": '"rtc_d3d12/cp_base.h"',
    "<rex/graphics/graphics_system.h>": '"rtc_d3d12/cp_base.h"',
    "<rex/ui/d3d12/d3d12_presenter.h>": '"rtc_d3d12/cp_base.h"',
    "<rex/graphics/d3d12/pipeline_cache.h>": '"rtc_d3d12/pipeline_cache.h"',
    "<rex/graphics/d3d12/primitive_processor.h>": '"rtc_d3d12/primitive_processor.h"',
    "<rex/graphics/primitive_processor.h>": '"rtc_d3d12/primitive_processor_base.h"',
    "<rex/graphics/d3d12/shader.h>": '"rtc_d3d12/shader.h"',
    "<rex/ui/d3d12/d3d12_descriptor_heap_pool.h>": '"rtc_d3d12/d3d12_descriptor_heap_pool.h"',''')
open(p, "w", encoding="utf-8").write(s)
print("ok")
