#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>

#include "dispatcher.h"
#include "shell_builtins.h"
#include "parser.h"

int setup_io(struct command *current_cmd, int *input_fd, int *output_fd, int *curr_pipe);

void execute_child_process(struct command *current_cmd, int input_fd, int output_fd, int *curr_pipe);

void handle_parent_process(struct command *current_cmd, int *input_fd, int *prev_pipe, int *curr_pipe);

int setup_io(struct command *current_cmd, int *input_fd, int *output_fd, int *curr_pipe) {
	if (current_cmd->output_type == COMMAND_OUTPUT_PIPE) {
		if (pipe(curr_pipe) == -1) {
			perror("pipe failed");
			return -1;
		}
		*output_fd = curr_pipe[1];
	} else if (current_cmd->output_type == COMMAND_OUTPUT_FILE_TRUNCATE) {
		*output_fd = open(current_cmd->output_filename, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	} else if (current_cmd->output_type == COMMAND_OUTPUT_FILE_APPEND) {
		*output_fd = open(current_cmd->output_filename, O_WRONLY | O_CREAT | O_APPEND, 0644);
	}
		
	// Check for input redirection < 
	if (current_cmd->input_filename) {
		*input_fd = open(current_cmd->input_filename, O_RDONLY);
		if (*input_fd == -1) {
			perror("Failed to open input file for reading");
			return -1;
		}
	}

	if ((*output_fd == -1) && (current_cmd->output_type == COMMAND_OUTPUT_FILE_TRUNCATE || current_cmd->output_type == COMMAND_OUTPUT_FILE_APPEND)) {
		perror("Failed to open output file");
		return -1;
	}

	// success
	return 0; 
}

void execute_child_process(struct command *current_cmd, int input_fd, int output_fd, int *curr_pipe) {
	// redirect stdin and/or output if necessary
	if (input_fd != STDIN_FILENO) {
		dup2(input_fd, STDIN_FILENO);
		close(input_fd);
	}
	if (output_fd != STDOUT_FILENO) {
		dup2(output_fd, STDOUT_FILENO);
		close(output_fd);
	}

	// Close unused write end of the current pipe
	if (current_cmd->output_type == COMMAND_OUTPUT_PIPE) {
		close(curr_pipe[1]); 
	}

	execvp(current_cmd->argv[0], current_cmd->argv);

	perror("execvp failed");
	exit(-1);
}

void handle_parent_process(struct command *current_cmd, int *input_fd, int *prev_pipe, int *curr_pipe) {
	// close write end in parent if current command output is a pipe
	if (current_cmd->output_type == COMMAND_OUTPUT_PIPE) {
		close(curr_pipe[1]);
	}

	// Close read end of previous pipe
	if (prev_pipe[0] != -1) {
		close(prev_pipe[0]);
	}

	// Set the input FD to the read end of curr pipe
	*input_fd = (current_cmd->output_type == COMMAND_OUTPUT_PIPE) ? curr_pipe[0] : STDIN_FILENO;

	// Copy current pipe to prev pipe for next iteration
	prev_pipe[0] = curr_pipe[0];
	prev_pipe[1] = curr_pipe[1];
}

/**
 * dispatch_external_command() - run a pipeline of commands
 *
 * @pipeline:   A "struct command" pointer representing one or more
 *              commands chained together in a pipeline.  See the
 *              documentation in parser.h for the layout of this data
 *              structure.  It is also recommended that you use the
 *              "parseview" demo program included in this project to
 *              observe the layout of this structure for a variety of
 *              inputs.
 *
 * Note: this function should not return until all commands in the
 * pipeline have completed their execution.
 *
 * Return: The return status of the last command executed in the
 * pipeline.
 */
static int dispatch_external_command(struct command *pipeline)
{
	/*
	 * Note: this is where you'll start implementing the project.
	 *
	 * It's the only function with a "TODO".  However, if you try
	 * and squeeze your entire external command logic into a
	 * single routine with no helper functions, you'll quickly
	 * find your code becomes sloppy and unmaintainable.
	 *
	 * It's up to *you* to structure your software cleanly.  Write
	 * plenty of helper functions, and even start making yourself
	 * new files if you need.
	 *
	 * For D1: you only need to support running a single command
	 * (not a chain of commands in a pipeline), with no input or
	 * output files (output to stdout only).  In other words, you
	 * may live with the assumption that the "input_file" field in
	 * the pipeline struct you are given is NULL, and that
	 * "output_type" will always be COMMAND_OUTPUT_STDOUT.
	 *
	 * For D2: you'll extend this function to support input and
	 * output files, as well as pipeline functionality.
	 *
	 * Good luck!
	 */

	pid_t pid;
	int status;

	// File descriptors for input/output redir. 
	int input_fd = STDIN_FILENO;
	int output_fd = STDOUT_FILENO;

	int prev_pipe[2] = {-1, -1};
	int curr_pipe[2];

	struct command *current_cmd = pipeline;

	while (current_cmd) {
		if (setup_io(current_cmd, &input_fd, &output_fd, curr_pipe) == -1) {
			return -1;
		}

		pid = fork();

		if (pid == 0) {
			// Child process
			execute_child_process(current_cmd, input_fd, output_fd, curr_pipe);
		} else if (pid > 0) {
			// Parent process
			handle_parent_process(current_cmd, &input_fd, prev_pipe, curr_pipe);
			current_cmd = current_cmd->pipe_to; // move to next command
		} else {
			perror("fork failed");
			return -1;
		}
	}

	// Wait for all child processes to complete
	while(wait(&status) > 0);

	return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

/**
 * dispatch_parsed_command() - run a command after it has been parsed
 *
 * @cmd:                The parsed command.
 * @last_rv:            The return code of the previously executed
 *                      command.
 * @shell_should_exit:  Output parameter which is set to true when the
 *                      shell is intended to exit.
 *
 * Return: the return status of the command.
 */
static int dispatch_parsed_command(struct command *cmd, int last_rv,
				   bool *shell_should_exit)
{
	/* First, try to see if it's a builtin. */
	for (size_t i = 0; builtin_commands[i].name; i++) {
		if (!strcmp(builtin_commands[i].name, cmd->argv[0])) {
			/* We found a match!  Run it. */
			return builtin_commands[i].handler(
				(const char *const *)cmd->argv, last_rv,
				shell_should_exit);
		}
	}

	/* Otherwise, it's an external command. */
	return dispatch_external_command(cmd);
}

int shell_command_dispatcher(const char *input, int last_rv,
			     bool *shell_should_exit)
{
	int rv;
	struct command *parse_result;
	enum parse_error parse_error = parse_input(input, &parse_result);

	if (parse_error) {
		fprintf(stderr, "Input parse error: %s\n",
			parse_error_str[parse_error]);
		return -1;
	}

	/* Empty line */
	if (!parse_result)
		return last_rv;

	rv = dispatch_parsed_command(parse_result, last_rv, shell_should_exit);
	free_parse_result(parse_result);
	return rv;
}
