import os
import sys
from clang.cindex import Index, Config, CursorKind, CompilationDatabase, TranslationUnit
from collections import defaultdict

LIBCLANG_DIR = '/opt/homebrew/opt/llvm/lib' 

try:
    if os.path.exists(os.path.join(LIBCLANG_DIR, 'libclang.dylib')):
        Config.set_library_path(LIBCLANG_DIR)
        print(f"✅ libclang 路径已设置: {LIBCLANG_DIR}")
    else:
        print(f"⚠️ 警告: 在预期路径 {LIBCLANG_DIR} 中未找到 libclang.dylib。将尝试使用默认系统路径。")
except Exception as e:
    print(f"❌ 设置 libclang 路径时发生错误: {e}")

# 设置系统包含路径（用于 C++ 标准库和系统头）
# 这些路径在 libclang 中不会自动设置，需要手动指定
CLANG_INCLUDE_PATHS = [
    '/opt/homebrew/Cellar/llvm/20.1.8/include/c++/v1',
    '/opt/homebrew/Cellar/llvm/20.1.8/lib/clang/20/include',
    '/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk/usr/include',
]

os.environ['CPATH'] = ':'.join(CLANG_INCLUDE_PATHS)
os.environ['CPLUS_INCLUDE_PATH'] = ':'.join(CLANG_INCLUDE_PATHS)


class CodeGraphGenerator:
    def __init__(self, compile_db_path='.'):
        """
        初始化代码图谱生成器。
        :param compile_db_path: 包含 compile_commands.json 的目录路径。
        """
        self.compile_db_path = compile_db_path
        self.nodes = defaultdict(dict)  # {UID: {name, kind, file_path}}
        self.relationships = []  # [(source_uid, target_uid, type)]
        self.next_uid = 1

    def _get_uid(self, cursor):
        """
        为给定的游标生成一个稳定且唯一的ID (UID)。
        我们使用游标的原始指针地址作为唯一 ID。
        """
        # 使用 Cursor 的 USR (如果存在) 作为稳定 ID 的基础；回退到对象 id
        try:
            usr = cursor.get_usr()
        except Exception:
            usr = None

        if usr:
            return hash(usr)
        # 回退：使用 cursor 对象 id，保证不会抛异常
        return hash(str(id(cursor)))

    def _is_user_code(self, file_path):
        """
        检查文件路径是否属于用户代码（maincpp目录）。
        排除系统库和外部依赖。
        """
        if not file_path or file_path == 'N/A':
            return False
        
        # 只包含 maincpp 目录下的文件
        if 'maincpp' in file_path:
            return True
        
        return False

    def _add_node(self, cursor, label, name, properties=None):
        """
        向图谱中添加一个节点。
        """
        # 检查文件是否在用户代码目录中
        file_path = cursor.location.file.name if cursor.location.file else 'N/A'
        if not self._is_user_code(file_path):
            return None
        
        uid = self._get_uid(cursor)
        if uid not in self.nodes:
            # 尝试获取定义，如果不是定义 (例如: 声明)，则跳过，避免重复节点
            definition = cursor.get_definition()
            if definition and definition != cursor:
                def_file = definition.location.file.name if definition.location.file else 'N/A'
                if self._is_user_code(def_file):
                    return self._get_uid(definition)
                else:
                    return None

            # 节点属性
            node_data = {
                'id': uid,
                'label': label,
                'name': name,
                'file_path': file_path,
                'line': cursor.location.line,
                **(properties if properties else {})
            }
            self.nodes[uid] = node_data
        return uid

    def _add_relationship(self, source_uid, target_uid, rel_type):
        """
        向图谱中添加一个关系。
        """
        # 避免重复关系
        rel_tuple = (source_uid, target_uid, rel_type)
        if rel_tuple not in self.relationships:
            self.relationships.append(rel_tuple)

    def visit(self, cursor, parent_uid=None):
        """
        递归访问 AST 游标。
        """
        node_kind = cursor.kind
        node_name = cursor.spelling

        current_uid = None

        # --- 1. 实体 (节点) 提取 ---
        if node_kind in (CursorKind.CLASS_DECL, CursorKind.STRUCT_DECL):
            # 类和结构体
            current_uid = self._add_node(cursor, 'Class', node_name)
        
        elif node_kind in (CursorKind.FUNCTION_DECL, CursorKind.CXX_METHOD):
            # 函数和方法
            try:
                signature = ' '.join([t.kind.spelling for t in cursor.type.argument_types()])
            except:
                # 如果无法获取参数类型，使用空签名
                signature = ''
            current_uid = self._add_node(cursor, 'Function', node_name, 
                                         {'signature': signature})
        
        # --- 2. 关系提取 ---

        # (a) 包含关系 (Containment)
        if parent_uid and current_uid:
            self._add_relationship(parent_uid, current_uid, 'CONTAINS')
        
        # (b) 调用关系 (Calls) - 查找函数体内部的 CALL_EXPR
        if node_kind == CursorKind.CALL_EXPR:
            # 找到被调用函数的定义
            # get_definition() on a CALL_EXPR may return None; try referencing children to find the callee
            callee_cursor = None
            try:
                callee_cursor = cursor.get_definition()
            except Exception:
                callee_cursor = None

            if not callee_cursor:
                # 尝试通过子节点找到引用 (DECL_REF_EXPR / MEMBER_REF_EXPR)
                for c in cursor.get_children():
                    if c.referenced is not None:
                        callee_cursor = c.referenced
                        break

            if callee_cursor and callee_cursor.kind in (CursorKind.FUNCTION_DECL, CursorKind.CXX_METHOD):
                # 使用 _add_node 来确保节点存在并返回 UID
                callee_uid = self._add_node(callee_cursor, 'Function', callee_cursor.spelling)
                # 只有当 callee 在用户代码中时才添加关系
                if parent_uid and callee_uid and self.nodes.get(parent_uid, {}).get('label') == 'Function':
                    self._add_relationship(parent_uid, callee_uid, 'CALLS')
                    return # CALL_EXPR 是叶子节点，不需要递归

        # --- 3. 递归 ---
        for child in cursor.get_children():
            # 关键：确保当前 UID 是一个有效的父节点 (例如 Class 或 Function)
            effective_parent_uid = current_uid if current_uid else parent_uid
            self.visit(child, effective_parent_uid)

    def _extract_compile_args(self, compile_command):
        """
        从编译命令中提取包含路径和编译器选项。
        返回适合 libclang 的参数列表。
        """
        args = []
        
        # compile_command.arguments might be a generator, so convert to list
        try:
            command_args = list(compile_command.arguments)
        except:
            command_args = compile_command.arguments
        
        skip_next = False
        i = 0
        while i < len(command_args):
            arg = command_args[i]
            
            if skip_next:
                skip_next = False
                i += 1
                continue
            
            # 跳过编译器和输出相关的参数
            if arg.endswith('.sh'):  # cc_wrapper.sh
                i += 1
                continue
            if arg in ['-c', '-o', '-MD', '-MF']:
                skip_next = True
                i += 1
                continue
            if arg.endswith('.o') or arg.endswith('.d'):
                i += 1
                continue
            
            # 提取包含路径
            if arg in ['-iquote', '-isystem', '-I']:
                if i + 1 < len(command_args):
                    path_arg = command_args[i + 1]
                    if not path_arg.startswith('-'):
                        args.append(arg)
                        args.append(path_arg)
                        i += 2
                        continue
            elif arg.startswith('-I'):
                # 例如 -I/path/to/include
                args.append(arg)
                i += 1
                continue
            elif arg.startswith('-iquote'):
                args.append(arg)
                i += 1
                continue
            elif arg.startswith('-isystem'):
                args.append(arg)
                i += 1
                continue
            
            # 保留其他重要的编译标志
            elif arg.startswith('-std=') or arg.startswith('-D'):
                args.append(arg)
            
            i += 1
        
        return args

    def generate(self):
        """
        主生成函数，加载编译数据库并开始解析。
        """
        try:
            db = CompilationDatabase.fromDirectory(self.compile_db_path)
            index = Index.create()
        except Exception as e:
            print(f"错误: 无法加载 compile_commands.json。请检查路径 '{self.compile_db_path}' 是否正确，并确保文件存在。")
            print(f"原始错误: {e}")
            return

        commands = db.getAllCompileCommands()
        print(f"成功加载 {len(commands)} 个文件的编译命令。")

        # 收集已处理的文件，避免重复解析
        processed_files = set()
        
        # 遍历所有文件并解析
        for compile_command in commands:
            file_path = compile_command.filename
            
            # 避免重复解析相同的文件
            if file_path in processed_files:
                continue
            processed_files.add(file_path)
            
            try:
                # 从编译命令中提取参数（虽然libclang Python绑定可能不支持所有参数）
                compile_args = self._extract_compile_args(compile_command)
                
                # 打印调试信息
                if compile_args:
                    print(f"   使用编译参数 ({len(compile_args)} 项): {compile_args[:3]}...")
                
                # libclang Python 绑定的 parse() 方法的 options 参数只接受整数标志，不接受列表
                # 所以我们只能使用默认解析
                tu = index.parse(file_path)
            except Exception as e3:
                print(f"   跳过此文件 {file_path}: {e3}")
                continue

            # -------------------------------------------------------------
            
            if not tu:
                print(f"警告: 无法解析 {file_path} (tu is None)")
                continue

            # 打印诊断信息（如果有）以帮助调试解析问题
            if hasattr(tu, 'diagnostics') and tu.diagnostics:
                print(f"诊断 ({file_path}):")
                for d in tu.diagnostics:
                    try:
                        # 仅打印错误和致命错误，以减少噪音
                        if d.severity >= 3: # 3: Error, 4: Fatal
                            print(f"  - {d.severity}: {d.spelling}")
                    except Exception:
                        print(f"  - {d}")

            # 为当前文件创建文件节点
            file_uid = self._add_node(tu.cursor, 'File', os.path.basename(file_path),
                                    {'path': file_path})

            # 开始遍历 AST
            print(f"解析文件: {file_path}...")
            self.visit(tu.cursor, file_uid)

        print("\n--- 解析完成 ---")
        print(f"提取了 {len(self.nodes)} 个节点和 {len(self.relationships)} 个关系。")
            
    def _escape_csv(self, value):
        """
        转义 CSV 值。
        """
        if value is None:
            value = ''
        else:
            value = str(value)
        
        # 如果包含逗号、引号或换行符，则用引号括起来
        if ',' in value or '"' in value or '\n' in value:
            value = '"' + value.replace('"', '""') + '"'
        return value

    def export_data(self):
        """
        以 Neo4j 导入格式 (CSV 样式) 打印节点和关系数据。
        """
        # 1. 打印节点数据 (CSV 格式)
        print("\n### 节点数据 (nodes.csv) ###")
        print(":ID,name,label,file_path,line,signature:string")
        for uid, data in self.nodes.items():
            signature = self._escape_csv(data.get('signature', ''))
            name = self._escape_csv(data['name'])
            label = self._escape_csv(data['label'])
            file_path = self._escape_csv(data['file_path'])
            line = data['line']
            print(f"{uid},{name},{label},{file_path},{line},{signature}")

        # 2. 打印关系数据 (CSV 格式)
        print("\n### 关系数据 (relationships.csv) ###")
        print(":START_ID,:END_ID,:TYPE")
        for start, end, rel_type in self.relationships:
            print(f"{start},{end},{rel_type}")

# --- 主执行逻辑 ---
if __name__ == "__main__":
    # 假设 compile_commands.json 在当前目录
    current_dir = os.getcwd()
    
    print(f"尝试加载编译数据库目录的绝对路径: {current_dir}")
    
    try:
        generator = CodeGraphGenerator(compile_db_path=current_dir)
        generator.generate()
        
        # 如果 generate 成功，则导出数据
        if generator.nodes:
            generator.export_data()
        else:
            print("❌ 错误: 未提取到任何节点。请检查 compile_commands.json 是否有效。")

    except Exception as e:
        print(f"❌ 运行过程中发生未捕获的错误: {e}")