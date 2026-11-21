# 运行 CMD 和参数，检查：
#   ARGS   —— 参数，用 "|" 分隔（不能用 ";"：add_test 会把它拆成多个参数，导致参数被悄悄丢掉）
#   EXPECT —— 退出码（CTest 本身只能区分成功/失败，区分不了 1 和 2）
#   REQUIRE —— 可选，输出中必须出现的正则（用来确认真的处理了指定的文件）
#   FORBID  —— 可选，输出（stdout + stderr）中不能出现的正则
string(REPLACE "|" ";" arg_list "${ARGS}")
execute_process(COMMAND ${CMD} ${arg_list} RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
set(all "${out}${err}")
if(NOT rc EQUAL EXPECT)
  message(FATAL_ERROR "退出码应该是 ${EXPECT}，实际是 ${rc}\n${all}")
endif()
if(DEFINED REQUIRE AND NOT all MATCHES "${REQUIRE}")
  message(FATAL_ERROR "输出中应该出现（${REQUIRE}），实际：\n${all}")
endif()
if(DEFINED FORBID AND all MATCHES "${FORBID}")
  message(FATAL_ERROR "输出中出现了不应该出现的内容（${FORBID}）：\n${all}")
endif()
