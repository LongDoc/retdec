file(READ
	"${DOXYGEN_LOG}"
	DOXYGEN_LOG_CONTENT
)

message(STATUS "${DOXYGEN_LOG_CONTENT}")