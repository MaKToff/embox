/**
 * @file
 * @brief A simple utility for tracking an input location (file:line) and
 *        accessing registered locations at run-time.
 *
 * @date 18.03.11
 * @author Eldar Abusalimov
 */

#ifndef UTIL_LOCATION_H_
#define UTIL_LOCATION_H_

struct location {
	const char *file;
	int line;
};

struct location_func {
	struct location at;
	const char *func;
};

#define LOCATION_INIT { \
		.file = __FILE__, \
		.line = __LINE__, \
	}

#define LOCATION_FUNC_INIT { \
		.at = LOCATION_INIT, \
		.func = __func__,       \
	}

#endif /* UTIL_LOCATION_H_ */
