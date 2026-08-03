import ast
import json
import sys

MAX_AST_NODES = 1024
MAX_CONSTANT_STRING = 64 * 1024
MAX_COMPREHENSIONS = 2
MAX_REPEAT = 100_000

ALLOWED_NODES = {
    ast.Expression, ast.Constant, ast.Name, ast.Load, ast.Store,
    ast.Subscript, ast.Slice,
    ast.List, ast.Tuple, ast.Dict, ast.Set, ast.Compare, ast.BoolOp, ast.BinOp,
    ast.UnaryOp, ast.IfExp, ast.ListComp, ast.SetComp, ast.DictComp,
    ast.GeneratorExp, ast.comprehension, ast.Call, ast.keyword,
    ast.Eq, ast.NotEq, ast.Lt, ast.LtE, ast.Gt, ast.GtE, ast.In, ast.NotIn,
    ast.And, ast.Or, ast.Not, ast.Add, ast.Sub, ast.Mult, ast.Div, ast.Mod,
}
PURE = {
    "len": len, "min": min, "max": max, "sum": sum, "sorted": sorted,
    "all": all, "any": any, "str": str, "int": int, "float": float,
    "bool": bool, "list": list, "dict": dict, "set": set, "tuple": tuple,
}

def reject(tree):
    nodes = list(ast.walk(tree))
    if len(nodes) > MAX_AST_NODES:
        raise ValueError("expression is too complex")
    comprehension_count = 0
    for node in nodes:
        if type(node) not in ALLOWED_NODES:
            raise ValueError("unsupported syntax: " + type(node).__name__)
        if isinstance(node, ast.Name) and node.id.startswith("__"):
            raise ValueError("dunder names are forbidden")
        if isinstance(node, ast.Constant) and isinstance(node.value, str):
            if len(node.value) > MAX_CONSTANT_STRING:
                raise ValueError("string constant is too large")
        if isinstance(node, ast.Call):
            if not isinstance(node.func, ast.Name) or node.func.id not in PURE:
                raise ValueError("only whitelisted pure functions are callable")
        if isinstance(node, ast.comprehension):
            comprehension_count += 1
            if node.is_async:
                raise ValueError("async comprehensions are forbidden")
            if not isinstance(node.target, ast.Name):
                raise ValueError("comprehension targets must be simple names")
        if isinstance(node, ast.BinOp) and isinstance(node.op, ast.Mult):
            if isinstance(node.left, (ast.List, ast.Tuple, ast.Dict, ast.Set)) or isinstance(
                node.right, (ast.List, ast.Tuple, ast.Dict, ast.Set)
            ):
                raise ValueError("collection repetition is forbidden")
            constants = [
                operand.value
                for operand in (node.left, node.right)
                if isinstance(operand, ast.Constant)
            ]
            if any(isinstance(value, (str, bytes)) for value in constants):
                raise ValueError("string repetition is forbidden")
            if any(
                isinstance(value, int) and not isinstance(value, bool)
                and abs(value) > MAX_REPEAT
                for value in constants
            ):
                raise ValueError("multiplication operand is too large")
    if comprehension_count > MAX_COMPREHENSIONS:
        raise ValueError("too many comprehension clauses")

try:
    request = json.loads(sys.stdin.buffer.read().decode("utf-8-sig"))
    if not isinstance(request, dict) or not isinstance(request.get("data"), dict):
        raise ValueError("request data must be an immutable JSON object")
    expression = request.get("expression", "")
    tree = ast.parse(expression, mode="eval")
    reject(tree)
    scope = {"data": request.get("data"), **PURE}
    result = eval(
        compile(tree, "<inspect>", "eval"),
        {"__builtins__": {}, **scope},
        {},
    )
    print(json.dumps({"ok": True, "result": result}, ensure_ascii=False))
except Exception as exc:
    print(json.dumps({"ok": False, "error": str(exc)}, ensure_ascii=False))
