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
	MESSAGE_QUEUE_HANDLE message_queue;
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
			mq_item->message_queue = message_queue;
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

static bool find_list_item_by_ptr_addr(LIST_ITEM_HANDLE list_item, const void* match_context)
{
	const void* list_item_value = singlylinkedlist_item_get_value(list_item);

	return (list_item_value == match_context);
}

static void on_message_processing_completed_callback(MESSAGE_HANDLE message, MESSAGE_QUEUE_RESULT result, void* reason, void* context)
{
	MESSAGE_QUEUE_ITEM* mq_item = (MESSAGE_QUEUE_ITEM*)context;

	LIST_ITEM_HANDLE list_item = singlylinkedlist_find(mq_item->message_queue->in_progress, find_list_item_by_ptr_addr, mq_item);

	if (list_item != NULL)
	{
		(void)singlylinkedlist_remove(mq_item->message_queue->in_progress, list_item);

		if (mq_item->message_queue->on_message_processing_completed_callback != NULL)
		{
			mq_item->message_queue->on_message_processing_completed_callback(
				mq_item->message, result, reason, mq_item->message_queue->on_message_processing_completed_context);
		}
		
		free(mq_item);
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
					on_message_processing_completed_callback(mq_item->message, MESSAGE_QUEUE_TIMEOUT, NULL, (void*)message_queue);
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
					on_message_processing_completed_callback(mq_item->message, MESSAGE_QUEUE_TIMEOUT, NULL, (void*)message_queue);
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
					on_message_processing_completed_callback(mq_item->message, MESSAGE_QUEUE_TIMEOUT, NULL, (void*)message_queue);
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
			LogError("failed moving message %p out of pending list", mq_item->message);
			on_message_processing_completed_callback(mq_item->message, MESSAGE_QUEUE_ERROR, NULL, (void*)mq_item);
			break; // Trying to avoid an infinite loop
		}
		else if ((mq_item->processing_start_time = get_time(NULL)) == INDEFINITE_TIME)
		{
			LogError("failed setting message %p processing_start_time", mq_item->message);
			on_message_processing_completed_callback(mq_item->message, MESSAGE_QUEUE_ERROR, NULL, (void*)mq_item);
		}
		else if (singlylinkedlist_add(message_queue->in_progress, (const void*)mq_item) != 0)
		{
			LogError("failed moving message %p to in-progress list", mq_item->message);
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
			MESSAGE_QUEUE_ITEM* mq_item = singlylinkedlist_item_get_value(list_item);

			(void)singlylinkedlist_remove(message_queue->in_progress, list_item);

			//on_message_processing_completed_callback()
		}
	}
}

OPTIONHANDLER_HANDLE retrieve_options(MESSAGE_QUEUE_HANDLE handle)
{

}