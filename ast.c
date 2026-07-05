/* =============================================================
 * ast.c - AST(ast.json) 구조 분석기
 *
 * [과제 목표]
 * 멘토님이 target.c를 pycparser로 분석해서 만들어준 ast.json을
 * 읽어서, 함수 개수 / 함수 이름 / 리턴타입 / 파라미터 / if문 개수를
 * 자동으로 뽑아내는 프로그램을 만드는 것.
 *
 * ast.json은 사람이 직접 읽기엔 너무 길고 복잡해서(수천 줄),
 * 프로그램이 대신 읽고 필요한 정보만 뽑아내게 만드는 게 핵심.
 * 이게 실제 정적분석 도구(CodeQL 등)가 하는 일의 축소판.
 * ============================================================= */

// json_c.c 안에 json을 다루는 함수들(json_create, json_get 등)이
// 이미 다 구현되어 있어서, 이 한 줄로 그 도구들을 전부 가져다 씀.
// 내가 json 파싱 로직을 처음부터 짤 필요 없음.
#include "json_c.c"
#include <windows.h>


/* -------------------------------------------------------------
 * [문제 상황]
 * json_c.c 헤더를 보면 json_read()가 선언만 있고 구현은 없음
 * ( //TODO read json file 이라고 써있음 )
 * json_create()는 "문자열"을 받아서 파싱해주는 함수인데,
 * 정작 "파일 -> 문자열"로 바꿔주는 부분이 없어서 직접 만들어야 했음.
 *
 * [해결 방법]
 * 1. fopen으로 파일을 열고
 * 2. fseek/ftell로 파일 크기를 미리 알아내고
 *    (파일 끝(SEEK_END)으로 이동한 다음 ftell로 현재 위치를 물어보면
 *     그게 곧 "파일 전체 크기"가 됨. 그다음 SEEK_SET으로 다시 처음으로)
 * 3. 그 크기만큼 메모리를 malloc으로 할당해서
 * 4. fread로 파일 내용을 통째로 문자열에 읽어담고
 * 5. 마지막에 '\0'을 붙여서 진짜 C 문자열로 만든 다음
 * 6. json_create()에 넘겨서 파싱 시작
 * ------------------------------------------------------------- */
json_value my_json_read(const char *path) {
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        fprintf(stderr, "파일을 열 수 없습니다: %s\n", path);
        exit(1);
    }

    fseek(fp, 0, SEEK_END);      // 파일 끝으로 이동
    long size = ftell(fp);       // 현재 위치 = 파일 전체 크기
    fseek(fp, 0, SEEK_SET);      // 다시 파일 처음으로 이동

    char *buffer = (char *)malloc(size + 1); // +1은 문자열 끝 '\0' 자리
    fread(buffer, 1, size, fp);
    buffer[size] = '\0';
    fclose(fp);

    json_value v = json_create(buffer); // 진짜 파싱은 여기서 일어남
    return v;
}


/* -------------------------------------------------------------
 * [문제 상황]
 * ast.json을 열어보니 함수의 리턴타입/파라미터 타입이 한 번에
 * 안 나오고 여러 단계를 거쳐서 나온다는 걸 발견함.
 *
 * 예) 그냥 int인 경우:
 *   FuncDecl -> TypeDecl -> IdentifierType -> names: ["int"]
 *
 * 예) 포인터(char*)인 경우, 중간에 PtrDecl이 하나 더 껴있음:
 *   FuncDecl -> PtrDecl -> TypeDecl -> IdentifierType -> names: ["char"]
 *
 * [해결 방법]
 * "지금 이 노드가 뭔지 확인 -> 아니면 한 단계 더 들어가서 다시 확인"을
 * 반복해야 해서, 함수가 자기 자신을 다시 호출하는 방식(재귀)으로 짬.
 * - PtrDecl을 만나면: 포인터니까 결과 뒤에 " *" 를 붙이고 한 단계 더 들어감
 * - ArrayDecl을 만나면: 배열이니까 결과 뒤에 "[]" 를 붙이고 한 단계 더 들어감
 * - TypeDecl을 만나면: 그냥 한 단계 더 들어감 (타입 정보 자체는 없음)
 * - IdentifierType을 만나면: 진짜 타입 이름이 여기 있으니까 꺼내고 종료
 * ------------------------------------------------------------- */
void resolve_type(json_value type_node, char *out) {
    // 혹시 객체가 아니면(예상 못한 구조면) 물음표로 표시하고 종료
    if (type_node.type != JSON_OBJECT) { strcpy(out, "?"); return; }

    char *nodetype = json_get_string(type_node, "_nodetype");

    if (strcmp(nodetype, "PtrDecl") == 0) {
        // 포인터 타입 -> 한 단계 더 들어가서(재귀) 안쪽 타입을 구하고 * 붙이기
        char inner[128] = "";
        resolve_type(json_get(type_node, "type"), inner);
        sprintf(out, "%s *", inner);
    }
    else if (strcmp(nodetype, "ArrayDecl") == 0) {
        // 배열 타입 -> 마찬가지로 재귀 호출 후 [] 붙이기
        char inner[128] = "";
        resolve_type(json_get(type_node, "type"), inner);
        sprintf(out, "%s[]", inner);
    }
    else if (strcmp(nodetype, "TypeDecl") == 0) {
        // 그냥 한 단계 감싸는 노드라서, 타입 자체는 안에 또 있음 -> 재귀
        resolve_type(json_get(type_node, "type"), out);
    }
    else if (strcmp(nodetype, "IdentifierType") == 0) {
        // 드디어 진짜 타입 이름이 나오는 지점 (여기서 재귀가 끝남)
        json_value names = json_get(type_node, "names");
        strcpy(out, json_get_string(names, 0));
    }
    else {
        strcpy(out, "?");
    }
}


/* -------------------------------------------------------------
 * [문제 상황]
 * if문 개수를 세려면 함수 몸통(body) 안에 있는 내용을 전부 뒤져야 함.
 * 근데 if문이 다른 if문 안에 중첩되어 있을 수도 있고, while문 안에
 * 들어있을 수도 있어서 "어디까지 파고들어야 할지" 미리 알 수가 없음.
 *
 * [해결 방법]
 * "객체를 만나면 그 안의 값들을 전부 다시 검사, 배열을 만나면 그 안의
 * 항목들을 전부 다시 검사"하는 식으로 끝까지 파고드는 재귀 함수를 만듦.
 *
 * 이때 json_c.c 헤더를 보다가 json_object 구조체가
 *   int last_index;
 *   char* keys[100];
 *   json_value values[100];
 * 이렇게 필드가 그대로 공개(public)되어 있는 걸 발견함.
 * 그래서 키 이름을 몰라도 obj->values[i] 로 안에 있는 값들을
 * 순서대로 전부 꺼내서 확인할 수 있다는 걸 이용함.
 * ------------------------------------------------------------- */
void count_if_recursive(json_value v, int *count) {
    if (v.type == JSON_OBJECT) {
        json_object *obj = (json_object *)v.value;

        // 지금 보고 있는 노드 자체가 If문인지 확인
        json_value nodetype = json_get_from_object(obj, "_nodetype");
        if (nodetype.type == JSON_STRING &&
            strcmp((char *)nodetype.value, "If") == 0) {
            (*count)++;
        }

        // 이 객체 안에 들어있는 모든 값들에 대해 똑같은 검사를 반복(재귀)
        for (int i = 0; i <= obj->last_index; i++) {
            count_if_recursive(obj->values[i], count);
        }
    }
    else if (v.type == JSON_ARRAY) {
        // 배열도 마찬가지로 안에 있는 모든 항목을 재귀적으로 검사
        json_array *arr = (json_array *)v.value;
        for (int i = 0; i <= arr->last_index; i++) {
            count_if_recursive(arr->values[i], count);
        }
    }
    // 문자열/숫자/불린/null 은 더 이상 내려갈 게 없으니 그냥 리턴(재귀 종료 조건)
}


/* =============================================================
 * [심화 과제] AST -> 원본 소스코드 복원 (가산점 10점)
 *
 * 기본 요구사항에서 뽑은 정보(이름/타입/파라미터)는 함수의 "겉모습"만
 * 재구성한 거였음. 여기서는 함수 몸통(body) 안의 실제 문장들까지
 * C 코드 문법으로 다시 조립해봄.
 *
 * 전부 다 복원하려면 노드 종류가 너무 많아서(BinaryOp, While, ArrayRef 등
 * ast.json에서 발견한 것만 20종류가 넘음), 가장 간단한 함수 3개
 * (main, error, be_push - 몸통이 한 줄짜리)만 골라서 복원해봄.
 * ============================================================= */

/* 식(expression)을 C 코드 문자열로 되돌리는 함수.
 * ID -> 변수/함수 이름 그대로
 * Constant -> 숫자나 문자열 값 그대로
 * FuncCall -> "함수이름(인자1, 인자2, ...)" 형태로 조립
 * ExprList -> 인자들을 콤마로 이어붙임 (FuncCall 안에서 쓰임) */
void render_expr(json_value e, char *out) {
    if (e.type == JSON_UNDEFINED || e.type == JSON_NULL) { out[0] = '\0'; return; }

    char *nodetype = json_get_string(e, "_nodetype");

    if (strcmp(nodetype, "ID") == 0) {
        strcpy(out, json_get_string(e, "name"));
    }
    else if (strcmp(nodetype, "Constant") == 0) {
        strcpy(out, json_get_string(e, "value"));
    }
    else if (strcmp(nodetype, "FuncCall") == 0) {
        char *fname = json_get_string(json_get(e, "name"), "name");
        json_value args = json_get(e, "args");
        char argstr[256] = "";
        if (args.type == JSON_OBJECT) {
            json_value exprs = json_get(args, "exprs");
            int n = json_len(exprs);
            for (int i = 0; i < n; i++) {
                char one[128] = "";
                render_expr(json_get(exprs, i), one);
                strcat(argstr, one);
                if (i < n - 1) strcat(argstr, ", ");
            }
        }
        sprintf(out, "%s(%s)", fname, argstr);
    }
    else {
        sprintf(out, "/* 지원 안 하는 식: %s */", nodetype);
    }
}

/* 문장(statement) 하나를 C 코드 한 줄로 되돌리는 함수.
 * Return -> "return 식;"
 * FuncCall(문장으로 쓰인 경우) -> "식;"
 * Compound -> 안에 있는 문장들을 한 줄씩 재귀적으로 출력 */
void render_stmt(json_value s, int indent) {
    if (s.type != JSON_OBJECT) return;
    char *nodetype = json_get_string(s, "_nodetype");

    for (int i = 0; i < indent; i++) printf("    ");

    if (strcmp(nodetype, "Return") == 0) {
        char expr[256] = "";
        render_expr(json_get(s, "expr"), expr);
        printf("return %s;\n", expr);
    }
    else if (strcmp(nodetype, "FuncCall") == 0) {
        char expr[256] = "";
        render_expr(s, expr);
        printf("%s;\n", expr);
    }
    else if (strcmp(nodetype, "Compound") == 0) {
        printf("{\n");
        json_value items = json_get(s, "block_items");
        if (items.type == JSON_ARRAY) {
            int n = json_len(items);
            for (int i = 0; i < n; i++) {
                render_stmt(json_get(items, i), indent + 1);
            }
        }
        for (int i = 0; i < indent; i++) printf("    ");
        printf("}\n");
    }
    else {
        printf("/* 지원 안 하는 문장: %s */\n", nodetype);
    }
}

/* 함수 하나를 통째로 복원해서 출력 (시그니처 + 몸통) */
void reconstruct_function(json_value funcdef) {
    json_value decl = json_get(funcdef, "decl");
    char *fname = json_get_string(decl, "name");

    char rettype[128] = "";
    resolve_type(json_get(decl, "type", "type"), rettype);

    printf("%s %s(", rettype, fname);
    json_value args = json_get(decl, "type", "args");
    if (args.type == JSON_OBJECT) {
        json_value params = json_get(args, "params");
        int pn = json_len(params);
        for (int p = 0; p < pn; p++) {
            json_value param = json_get(params, p);
            char *pname = json_get_string(param, "name");
            char ptype[128] = "";
            resolve_type(json_get(param, "type"), ptype);
            printf("%s %s%s", ptype, pname, (p < pn - 1) ? ", " : "");
        }
    }
    printf(") ");

    render_stmt(json_get(funcdef, "body"), 0);
}


/* -------------------------------------------------------------
 * [메인 흐름]
 * 1. ast.json을 읽어서 구조로 변환
 * 2. root 안의 "ext" 배열(최상위 함수/변수 목록)을 꺼냄
 * 3. ext 배열을 처음부터 끝까지 돌면서 각 항목을 확인
 * 4. "_nodetype"이 "FuncDef"(함수 정의)인 것만 골라서
 *    이름/리턴타입/파라미터/if개수를 뽑아 출력
 * 5. 마지막에 총 함수 개수 출력
 * 6. [심화] 가장 간단한 함수 3개(main, error, be_push) 원본 복원
 * ------------------------------------------------------------- */
int main(void) {
    SetConsoleOutputCP(CP_UTF8); // 콘솔 출력 코드페이지를 UTF-8로 강제 설정 (한글 깨짐 방지)
    json_value root = my_json_read("ast.json");
    json_value ext  = json_get(root, "ext"); // ext: 전역 선언+함수정의 배열
    int n = json_len(ext);                    // 배열 안에 총 몇 개 있는지

    int func_count = 0;

    for (int i = 0; i < n; i++) {
        json_value item = json_get(ext, i);
        char *nodetype = json_get_string(item, "_nodetype");

        // Decl(전역변수 선언)인 것들은 건너뛰고, FuncDef(함수 정의)만 처리
        if (strcmp(nodetype, "FuncDef") != 0) continue;
        func_count++;

        // 함수 이름은 decl.name 에 있음
        json_value decl  = json_get(item, "decl");
        char *fname      = json_get_string(decl, "name");

        // 리턴타입은 decl.type.type 부터 시작해서 타입 체인을 타고 내려가야 함
        char rettype[128] = "";
        resolve_type(json_get(decl, "type", "type"), rettype);

        printf("[%d] %s %s(", func_count, rettype, fname);

        // 파라미터 목록은 decl.type.args.params 배열에 들어있음
        // (파라미터가 없는 함수는 args 자체가 null이라서 확인 필요)
        json_value args = json_get(decl, "type", "args");
        if (args.type == JSON_OBJECT) {
            json_value params = json_get(args, "params");
            int pn = json_len(params);
            for (int p = 0; p < pn; p++) {
                json_value param = json_get(params, p);
                char *pname = json_get_string(param, "name");
                char ptype[128] = "";
                resolve_type(json_get(param, "type"), ptype);
                printf("%s %s%s", ptype, pname, (p < pn - 1) ? ", " : "");
            }
        }
        printf(")\n");

        // if문 개수는 함수 몸통(body) 전체를 재귀로 훑어서 셈
        int if_count = 0;
        json_value body = json_get(item, "body");
        count_if_recursive(body, &if_count);
        printf("    if문 개수: %d\n", if_count);
    }

    printf("\n총 함수 개수: %d\n", func_count);

    /* ---- [심화] 가장 간단한 함수 3개 원본 소스코드 복원 ---- */
    printf("\n=== 심화: 원본 소스코드 복원 (함수 3개) ===\n\n");
    const char *targets[3] = {"main", "error", "be_push"};
    for (int t = 0; t < 3; t++) {
        for (int i = 0; i < n; i++) {
            json_value item = json_get(ext, i);
            char *nodetype = json_get_string(item, "_nodetype");
            if (strcmp(nodetype, "FuncDef") != 0) continue;

            char *fname = json_get_string(json_get(item, "decl"), "name");
            if (strcmp(fname, targets[t]) == 0) {
                reconstruct_function(item);
                printf("\n");
                break;
            }
        }
    }

    return 0;
}