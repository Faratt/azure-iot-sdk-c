// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

#include <stdlib.h>
#include <stdbool.h>
#include "azure_c_shared_utility/optimize_size.h"
#include "azure_c_shared_utility/crt_abstractions.h"
#include "azure_c_shared_utility/gballoc.h"
#include "azure_c_shared_utility/agenttime.h" 
#include "azure_c_shared_utility/xlogging.h"
#include "azure_c_shared_utility/uniqueid.h"
#include "azure_c_shared_utility/singlylinkedlist.h"
#include "iothub_client_private.h"
#include "iothub_client_version.h"

typedef struct MESSAGE_QUEUE_TAG MESSAGE_QUEUE;

#include "message_queue.h"

#define RESULT_OK 0
#define INDEFINITE_TIME ((time_t)(-1))

const char* SAVED_OPTION_MAX_ENQUEUE_TIME_SECS = "SAVED_OPTION_MAX_ENQUEUE_TIME_SECS";
const char* SAVED_OPTION_MAX_PROCESSING_TIME_SECS = "SAVED_OPTION_MAX_PROCESSING_TIME_SECS";


struct MESSAGE_QUEUE_TAG
{
	double max_message_enqueued_time_secs;
	double max_message_processing_time_secs;
	
	PROCESS_MESSAGE_CALLBACK on_process_message_callback;
	void* on_process_message_context;
	
	MESSAGE_PROCESSING_COMPLETED_CALLBACK on_message_processing_completed_callback;
	void* on_message_processing_completed_context;

	SINGLYLINKEDLIST_HANDLE pending;
	SINGLYLINKEDLIST_HANDLE in_progress;
};

typedef struct MESSAGE_QUEUE_ITEM_TAG
{
	MESSAGE_HANDLE message;
	time_t enqueue_time;
	time_t processing_start_time;
} MESSAGE_QUEUE_ITEM;



void message_queue_destroy(MESSAGE_QUEUE_HANDLE message_queue)
{
	if (message_queue != NULL)
	{
		if (message_queue->pending != NULL)
		{
			singlylinkedlist_destroy(message_queue->pending);
		}

		if (message_queue->in_progress != NULL)
		{
			singlylinkedlist_destroy(message_queue->in_progress);
		}
	}
}

MESSAGE_QUEUE_HANDLE message_queue_create(MESSAGE_QUEUE_CONFIG* config)
{
	MESSAGE_QUEUE* result;

	if (config == NULL)
	{
		LogError("invalid configuration (NULL)");
		result = NULL;
	}
	else if (config->on_process_message_callback == NULL)
	{
		LogError("invalid configuration (on_process_message_callback is NULL)");
		result = NULL;
	}
	else if ((result = (MESSAGE_QUEUE*)malloc(sizeof(MESSAGE_QUEUE))) == NULL)
	{
		LogError("failed allocating MESSAGE_QUEUE");
		result = NULL;
	}
	else if ((result->pending = singlylinkedlist_create()) == NULL)
	{
		LogError("failed allocating MESSAGE_QUEUE pending list");
		message_queue_destroy(result);
		result = NULL;
	}
	else if ((result->in_progress = singlylinkedlist_create()) == NULL)
	{
		LogError("failed allocating MESSAGE_QUEUE in-progress list");
		message_queue_destroy(result);
		result = NULL;
	}
	else
	{
		result->on_process_message_callback = config->on_process_message_callback;
		result->on_process_message_context = config->on_process_message_context;
		result->on_message_processing_completed_callback = config->on_message_processing_completed_callback;
		result->on_message_processing_completed_context = config->on_message_processing_completed_context;
	}

	return result;
}

int message_queue_add(MESSAGE_QUEUE_HANDLE message_queue, MESSAGE_HANDLE message)
{
	int result;

	if (message_queue == NULL || message == NULL)
	{
		LogError("invalid argument (message_queue=%p, message=%p)", message_queue, message);
		result = NULL;
	}
	else
	{
		MESSAGE_QUEUE_ITEM* mq_item;

		if ((mq_item = (MESSAGE_QUEUE_ITEM*)malloc(sizeof(MESSAGE_QUEUE_ITEM))) == NULL)
		{
			LogError("failed creating container for message");
			result = __FAILURE__;
		}
		else if ((mq_item->enqueue_time = time(NULL)) == INDEFINITE_TIME)
		{
			LogError("failed setting message enqueue time");
			free(mq_item);
			result = __FAILURE__;
		}
		else if (singlylinkedlist_add(message_queue->pending, (const void*)message) == NULL)
		{
			LogError("failed enqueing message");
			free(mq_item);
			result = __FAILURE__;
		}
		else
		{
			mq_item->message = message;
			mq_item->processing_start_time = INDEFINITE_TIME;
			result = RESULT_OK;
		}
	}

	return result;
}

int message_queue_is_empty(MESSAGE_QUEUE_HANDLE message_queue, bool* is_empty)
{
	int result;

	if (message_queue == NULL || is_empty == NULL)
	{
		LogError("invalid argument (message_queue=%p, is_empty=%p)", message_queue, is_empty);
		result = __FAILURE__;
	}
	else
	{
		*is_empty = (singlylinkedlist_get_head_item(message_queue->pending) == NULL && singlylinkedlist_get_head_item(message_queue->in_progress) == NULL);
		result = RESULT_OK;
	}

	return result;
}

static bool dequeue_message_and_fire_callback(MESSAGE_QUEUE_HANDLE message_queue, SINGLYLINKEDLIST_HANDLE list, MESSAGE_HANDLE message, MESSAGE_QUEUE_RESULT result, void* reason)
{
	LIST_ITEM_HANDLE list_item = singlylinkedlist_get_head_item(list);

	while (list_item != NULL)
	{
		MESSAGE_QUEUE_ITEM* mq_item = singlylinkedlist_item_get_value(list_item);

		if (mq_item->message == message)
		{
			(void)singlylinkedlist_remove(list, list_item);

			if (message_queue->on_message_processing_completed_callback != NULL)
			{
				message_queue->on_message_processing_completed_callback(mq_item->message, result, reason, message_queue->on_message_processing_completed_context);
			}

			free(mq_item);

			break;
		}
	}

	return (list_item != NULL);
}


static void on_message_processing_completed_callback(MESSAGE_HANDLE message, MESSAGE_QUEUE_RESULT result, void* reason, void* context)
{
	MESSAGE_QUEUE_HANDLE message_queue = (MESSAGE_QUEUE_HANDLE)context;

	if (!dequeue_message_and_fire_callback(message_queue, message_queue->in_progress, message, result, reason))
	{
		LogError("on_message_processing_completed_callback invoked for message not in in-progress list (%p)", message);
	}
}

static void process_timeouts(MESSAGE_QUEUE_HANDLE message_queue)
{
	time_t current_time;

	if ((current_time = get_time(NULL)) == INDEFINITE_TIME)
	{
		LogError("failed processing timeouts (get_time failed)");
	}
	else
	{
		if (message_queue->max_message_enqueued_time_secs > 0)
		{
			LIST_ITEM_HANDLE list_item = singlylinkedlist_get_head_item(message_queue->pending);

			while (list_item != NULL)
			{
				LIST_ITEM_HANDLE current_list_item = list_item;
				MESSAGE_QUEUE_ITEM* mq_item = (MESSAGE_QUEUE_ITEM*)singlylinkedlist_item_get_value(current_list_item);

				list_item = singlylinkedlist_get_next_item(list_item);

				if (mq_item == NULL)
				{
					LogError("failed processing timeouts (unexpected NULL pointer to MESSAGE_QUEUE_ITEM)");
				}
				else if (get_difftime(current_time, mq_item->enqueue_time) >= message_queue->max_message_enqueued_time_secs)
				{
					(void)dequeue_message_and_fire_callback(message_queue, message_queue->pending, mq_item->message, MESSAGE_QUEUE_TIMEOUT, NULL);
				}
				else
				{
					// The pending list order is already based on enqueue time, so if one message is not expired, later ones won't be either.
					break;
				}
			}

			list_item = singlylinkedlist_get_head_item(message_queue->in_progress);

			while (list_item != NULL)
			{
				LIST_ITEM_HANDLE current_list_item = list_item;
				MESSAGE_QUEUE_ITEM* mq_item = (MESSAGE_QUEUE_ITEM*)singlylinkedlist_item_get_value(current_list_item);

				list_item = singlylinkedlist_get_next_item(list_item);

				if (mq_item == NULL)
				{
					LogError("failed processing timeouts (unexpected NULL pointer to MESSAGE_QUEUE_ITEM)");
				}
				else if (get_difftime(current_time, mq_item->enqueue_time) >= message_queue->max_message_enqueued_time_secs)
				{
					(void)dequeue_message_and_fire_callback(message_queue, message_queue->in_progress, mq_item->message, MESSAGE_QUEUE_TIMEOUT, NULL);
				}
			}
		}

		if (message_queue->max_message_processing_time_secs > 0)
		{
			LIST_ITEM_HANDLE list_item = singlylinkedlist_get_head_item(message_queue->in_progress);

			while (list_item != NULL)
			{
				LIST_ITEM_HANDLE current_list_item = list_item;
				MESSAGE_QUEUE_ITEM* mq_item = (MESSAGE_QUEUE_ITEM*)singlylinkedlist_item_get_value(current_list_item);

				list_item = singlylinkedlist_get_next_item(list_item);

				if (mq_item == NULL)
				{
					LogError("failed processing timeouts (unexpected NULL pointer to MESSAGE_QUEUE_ITEM)");
				}
				else if (get_difftime(current_time, mq_item->processing_start_time) >= message_queue->max_message_processing_time_secs)
				{
					(void)dequeue_message_and_fire_callback(message_queue, message_queue->in_progress, mq_item->message, MESSAGE_QUEUE_TIMEOUT, NULL);
				}
				else
				{
					// The in-progress list order is already based on start-processing time, so if one message is not expired, later ones won't be either.
					break;
				}
			}
		}
	}
}

static void process_pending_messages(MESSAGE_QUEUE_HANDLE message_queue)
{
	LIST_ITEM_HANDLE list_item;

	while ((list_item = singlylinkedlist_get_head_item(message_queue->pending)) != NULL)
	{
		MESSAGE_QUEUE_ITEM* mq_item = (MESSAGE_QUEUE_ITEM*)singlylinkedlist_item_get_value(list_item);

		if (singlylinkedlist_remove(message_queue->pending, list_item) != 0)
		{
			LogError("failed moving message out of pending list (%p)", mq_item->message);
			on_message_processing_completed_callback(mq_item->message, MESSAGE_QUEUE_ERROR, NULL, (void*)mq_item);
			break; // Trying to avoid an infinite loop
		}
		else if ((mq_item->processing_start_time = get_time(NULL)) == INDEFINITE_TIME)
		{
			LogError("failed setting message processing_start_time (%p)", mq_item->message);
			on_message_processing_completed_callback(mq_item->message, MESSAGE_QUEUE_ERROR, NULL, (void*)mq_item);
		}
		else if (singlylinkedlist_add(message_queue->in_progress, (const void*)mq_item) != 0)
		{
			LogError("failed moving message to in-progress list (%p)", mq_item->message);
			on_message_processing_completed_callback(mq_item->message, MESSAGE_QUEUE_ERROR, NULL, (void*)mq_item);
		}
		else
		{
			message_queue->on_process_message_callback(mq_item->message, on_message_processing_completed_callback, (void*)mq_item);
		}
	}
}

void message_queue_do_work(MESSAGE_QUEUE_HANDLE message_queue)
{
	if (message_queue != NULL)
	{
		process_timeouts(message_queue);
		process_pending_messages(message_queue);
	}
}

void message_queue_remove_all(MESSAGE_QUEUE_HANDLE message_queue)
{
	if (message_queue != NULL)
	{
		LIST_ITEM_HANDLE list_item;

		while ((list_item = singlylinkedlist_get_head_item(message_queue->in_progress)) != NULL)
		{
			MESSAGE_QUEUE_ITEM* mq_item = (MESSAGE_QUEUE_ITEM*)singlylinkedlist_item_get_value(list_item);

			(void)dequeue_message_and_fire_callback(message_queue, message_queue->in_progress, mq_item->message, MESSAGE_QUEUE_CANCELLED, NULL);
		}

		while ((list_item = singlylinkedlist_get_head_item(message_queue->pending)) != NULL)
		{
			MESSAGE_QUEUE_ITEM* mq_item = (MESSAGE_QUEUE_ITEM*)singlylinkedlist_item_get_value(list_item);

			(void)dequeue_message_and_fire_callback(message_queue, message_queue->pending, mq_item->message, MESSAGE_QUEUE_CANCELLED, NULL);
		}
	}
}

static void* cloneOption(const char* name, const void* value) 
{
	void* result;

	if (name == NULL || value == NULL)
	{
		LogError("invalid argument (name=%p, value=%p)", name, value);
		result = NULL;
	}
	else if (strcmp(SAVED_OPTION_MAX_ENQUEUE_TIME_SECS, name) == 0 || strcmp(SAVED_OPTION_MAX_PROCESSING_TIME_SECS, name) == 0)
	{
		if ((result = malloc(sizeof(double))) == NULL)
		{
			LogError("failed cloning option %s (malloc failed)", name);
		}
		else
		{
			memcpy(result, value, sizeof(double));
		}
	}
	else
	{
		LogError("option %s is invalid", name);
		result = NULL;
	}

	return result;
}

static void destroyOption(const char* name, const void* value)
{
}

static int setOption(void* handle, const char* name, const void* value)
{

}

OPTIONHANDLER_HANDLE retrieve_options(MESSAGE_QUEUE_HANDLE message_queue)
{
	OPTIONHANDLER_HANDLE result;

	if (message_queue == NULL)
	{
		LogError("invalid argument (message_queue is NULL)");
		result = NULL;
	}
	else if ((result = OptionHandler_Create(cloneOption, destroyOption, setOption)) == NULL)
	{
		LogError("failed creating OPTIONHANDLER_HANDLE");
	}
	else if (OptionHandler_AddOption(result, SAVED_OPTION_MAX_ENQUEUE_TIME_SECS, &message_queue->max_message_enqueued_time_secs) != OPTIONHANDLER_OK)
	{
		LogError("failed retrieving options (failed adding %s)", SAVED_OPTION_MAX_ENQUEUE_TIME_SECS);
		OptionHandler_Destroy(result);
		result = NULL;
	}
	else if (OptionHandler_AddOption(result, SAVED_OPTION_MAX_PROCESSING_TIME_SECS, &message_queue->max_message_processing_time_secs) != OPTIONHANDLER_OK)
	{
		LogError("failed retrieving options (failed adding %s)", SAVED_OPTION_MAX_PROCESSING_TIME_SECS);
		OptionHandler_Destroy(result);
		result = NULL;
	}

	return result;
}