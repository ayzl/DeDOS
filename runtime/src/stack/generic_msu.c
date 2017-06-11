/**
 * @file generic_msu.c
 *
 *
 * Contains function for the creation of MSUs, as well as the
 * registration of new MSU types
 */

#include <time.h>
#include <stdlib.h>
#include "stats.h"
#include "generic_msu.h"
#include "runtime.h"
#include "routing.h"
#include "logging.h"
#include "dedos_statistics.h"
#include "data_plane_profiling.h"
#include "dedos_thread_queue.h"

// TODO: This type-registration should probably be handled elsewhere
//       probably in msu_tracker?

/** Maximum number of MSU types that can be registered */
#define MAX_MSU_TYPES 64
/** When msu types are registered, they are included in this array */
static struct msu_type *msu_types = NULL;
/** Number of currently-registered msu types */
static unsigned int n_types = 0;

/**
 * Tracks the amount of memory allocated to an MSU.
 * Private function
 * @param msu msu in which to track allocation
 * @param bytes number of bytes to malloc
 * @returns pointer to the allocated memory
 */
static void *msu_track_alloc(struct generic_msu *msu, size_t bytes) {
    void *ptr = NULL;
    ptr = malloc(bytes);
    if (!ptr) {
        log_error("Failed to allocate %d bytes for MSU id %d, %s",
            (int)bytes, msu->id, msu->type->name);
    } else {
        msu->stats.memory_allocated[1] += bytes;
        msu->stats.memory_allocated[0] = time(NULL);
        log_debug("Successfully allocated %d bytes for MSU id %d, \
                  %s memory footprint: %u bytes at time %d",
                  (int)bytes, msu->id, msu->type->name,
                  msu->stats.memory_allocated[1],
                  msu->stats.memory_allocated[0]);
    }
    return ptr;
}

/**
 * Tracks the amount of memory allocated to an MSU.
 * Private function
 * @param ptr memory to free
 * @param msu msu in which to track allocatioin
 * @param bytes number of bytes being freed
 */
static void msu_track_free(void* ptr, struct generic_msu * msu, size_t bytes) {
    msu->stats.memory_allocated[1] -= bytes;
    msu->stats.memory_allocated[0] = time(NULL);
    log_debug("Freeing %u bytes used by MSU id %d, %s, memory footprint: %u bytes at time %d",
              (int)bytes, msu->id, msu->type->name,
              msu->stats.memory_allocated[1],
              msu->stats.memory_allocated[0]);

    free(ptr);
}

/**
 * Registers an MSU type so that it can be later referenced by its ID.
 *
 * Adds the type to the static *msu_types structure.
 * If more than 64 different types of MSUs are registered, this will fail.
 * @param type MSU type to be registered.
 */
void register_msu_type(struct msu_type *type) {
    // TODO: realloc if n_types+1 == MAX
    if (msu_types == NULL)
        msu_types = malloc(sizeof(*msu_types) * MAX_MSU_TYPES);
    msu_types[n_types] = *type;
    log_debug("Registered MSU type: %s",type->name);
    n_types++;
}

/**
 * Gets a registered msu_type structure that matches the provided id.
 *
 * @param type_id numerical ID of the MSU type to be retrieved
 * @return first msu_type in which type->type_id matches the provided id or NULL if N/A
 */
struct msu_type *msu_type_by_id(unsigned int type_id){
    struct msu_type *type = NULL;
    // NOTE: This will require slightly more time than a switch statement
    for (int i=0; i<n_types; i++){
        if (msu_types[i].type_id == type_id){
            type = &msu_types[i];
            break;
        }
    }
    return type;
}

int msu_type_by_id_corrected(unsigned int type_id, struct msu_type **type){
    *type = NULL;
    // NOTE: This will require slightly more time than a switch statement
    for (int i=0; i<n_types; i++){
        if (msu_types[i].type_id == type_id){
            *type = &msu_types[i];
            return 0;
        }
    }
    return -1;
}

/**
 * Called on receipt of a new message from the global controller by an MSU.
 * Adds new routes and updates the scheduling weight if appropriate.
 * Calls individual MSU type's receive_ctrl() if not NULL
 * Automatically frees the queue_item and queue_item->buffer once processed.
 *
 * @param self MSU receiving the control message
 * @param queue_item control queue message
 * @return 0 on success, -1 on error
 */
int msu_receive_ctrl(struct generic_msu *self, struct generic_msu_queue_item *queue_item){
    struct msu_action_thread_data *update = queue_item->buffer;

    int rtn = 0;
    if (self->id != update->msu_id){
        log_error("ERROR: MSU %d got updated destined for MSU %d", self->id, update->msu_id);
        rtn = -1;
    } else {

        log_debug("MSU %d got update with type %u", self->id, update->action);
        int error = 0;
        switch(update->action){
            case ADD_ROUTE_TO_MSU:
                error = add_route_to_set(&self->routes, update->route_id);
                if ( error )
                    log_error("Error adding route to MSU %d", self->id);
                break;
            case DEL_ROUTE_FROM_MSU:
                error = del_route_from_set(&self->routes, update->route_id);
                if ( error )
                    log_error("Error removing route from MSU %d", self->id);
                break;
            default:
                error = -1;
        }
        if (self->type->receive_ctrl){
            error = (self->type->receive_ctrl(self, update)) == 0;
        }
        if (error){
            log_error("Error handling update type %u in MSU %d",
                    update->action, self->id);
        }
    }

    free(update);
    free(queue_item);
    log_debug("Freed update_msg and control_q_item %s","");
    return error;
}

/**
 *  Hidden data structure to protect access to MSU specific data.
 *  Data should only ever be allocated to this structure via:
 *      msu_data_alloc()
 *  and only freed via
 *      msu_data_free()
 */
struct msu_data{
    size_t n_bytes;
    void *data;
};

/** Allocates data in the msu and tracks the amount of allocated data.
 * Returns a pointer to the allocated data.
 * NOTE: MSU can only have a single allocated data structure at any time
 *       Calling this function again will realloc that data, and
 *       may cause unexpected behavior
 * TODO: Provide ID by which allocated data can be retrieved
 * @param msu MSU in which to track the allocated data
 * @param bytes number of bytes to malloc
 * @return pointer to the allocated data, or NULL if memory could not be allocated
 */
void *msu_data_alloc(struct generic_msu * msu, size_t bytes)
{
    void *ptr = realloc(msu->data_p->data, bytes);
    if (ptr == NULL){
        log_error("Failed to allocate %d bytes for MSU id %d, %s",
                  (int)bytes, msu->id, msu->type->name);
    } else {
        msu->stats.memory_allocated[1] += (bytes - msu->data_p->n_bytes);
        msu->stats.memory_allocated[0] = time(NULL);
        log_debug("Successfully allocated %d bytes for MSU id %d, "
                  "%s memory footprint: %u bytes at time %d",
                  (int)bytes, msu->id, msu->type->name,
                  msu->stats.memory_allocated[1],
                  msu->stats.memory_allocated[0]);

        msu->data_p->n_bytes = bytes;
        msu->data_p->data = ptr;
    }
    return ptr;
}

/**
 * Frees data stored within an MSU and tracks that the data has been freed.
 * @param msu MSU in which to track the freed data
 */
void msu_data_free(struct generic_msu *msu) {
    msu->stats.memory_allocated[1] -= msu->data_p->n_bytes;
    msu->stats.memory_allocated[0] = time(NULL);
    log_debug("Freeing %u bytes used by MSU id %d, %s, memory footprint: %u bytes at time %d",
              (int)msu->data_p->n_bytes, msu->id, msu->type->name,
              msu->stats.memory_allocated[1],
              msu->stats.memory_allocated[0]);

    free(msu->data_p->data);
}

/**
 *  Gets the data allocated within an MSU.
 *  @return pointer to the allocated data
 */
void *msu_data(struct generic_msu *msu) {
    return msu->data_p->data;
}

/**
 * Private function to allocate a new MSU.
 * MSU creation should be handled through init_msu()
 */
struct generic_msu *msu_alloc(void) {
    struct generic_msu *msu = malloc(sizeof(*msu));
    if (msu == NULL){
        log_error("%s", "Failed to allocate msu");
        return msu;
    }

    msu->stats.memory_allocated[1] = 0;
    msu->stats.queue_item_processed[1] = 0;

    int thread_index = get_thread_index(pthread_self());
    struct dedos_thread *dedos_thread = &all_threads[thread_index];
    if(!pthread_equal(dedos_thread->tid, pthread_self())){
        log_error("Unable to get correct pointer to self thread struct for msu queue init");
        return NULL;
    }

    msu->data_p = malloc(sizeof(struct msu_data));
    msu->data_p->n_bytes = 0;
    msu->data_p->data = NULL;

    msu->q_in.mutex = mutex_init();
    msu->q_in.shared = 1;
    generic_msu_queue_init(&msu->q_in, dedos_thread->q_sem);

    msu->q_control.mutex = mutex_init();
    msu->q_control.shared = 1;
    generic_msu_queue_init(&msu->q_control, dedos_thread->q_sem);
    return msu;
}

/**
 * Private function to free an MSU.
 * MSU destruction should be handled through destroy_msu()
 */
void msu_free(struct generic_msu* msu)
{
    free(msu->data_p);
    free(msu);
}

/**
 * Sends a message to the main thread queue indicating that
 * the requested MSU has failed to be created.
 */
int msu_failure(int msu_id){
    struct dedos_thread_msg *thread_msg = dedos_thread_msg_alloc();
    if (thread_msg){
        thread_msg->action = FAIL_CREATE_MSU;
        thread_msg->action_data = msu_id;
        thread_msg->buffer_len = 0;
        thread_msg->data = NULL;
        dedos_thread_enqueue(main_thread, thread_msg);
        return 0;
    }
    log_error("Failed to enqueue FAIL_CREATE_MSU message on main thread");
    return -1;
}


/**
 * Malloc's and creates a new MSU of the specified type and id.
 *
 * @param type_id ID of the MSU type to be created
 * @param msu_id ID of the instance of the MSU to be created
 * @param create_action Initial data to be provided to the MSU
 * @return initialized MSU or NULL if error occurred
 */
struct generic_msu *init_msu(unsigned int type_id, int msu_id,
                             struct create_msu_thread_data *create_action){
    if (type_id < 1){
        return NULL;
    }

    struct generic_msu *msu = msu_alloc();
    msu->id = msu_id;
    msu->type = msu_type_by_id(type_id);
    msu->data_p->data = create_action->init_data;
    log_debug("init data: %s",create_action->init_data);
    // TODO: queue_ws, msu_ws, what????

    // TODO: What's all that stuff about global pointers in the old code ???

    if (msu->type == NULL){
        /* for enqueing failure messages*/
        log_error("Unknown MSU type %d", type_id);
        msu_failure(msu_id);
        msu_free(msu);
        return NULL;
    }

    // Call the type-specific initialization function
    // TODO: What is this "creation_init_data"
    if (msu->type->init){
        int rtn = msu->type->init(msu, create_action);
        if (rtn){
            log_error("MSU creation failed for %s MSU: id %d",
                      msu->type->name, msu_id);
            msu_failure(msu_id);
            msu_free(msu);
            return NULL;
        }
    }
    log_info("Created %s MSU: id %d", msu->type->name, msu_id);
    msu->routes = NULL;
    msu->scheduling_weight = 1;
    msu->routing_state = NULL;
    return msu;
}

/** Frees an instance of an MSU along with associated structures.
 * Calls type-specific destructor if applicable.
 * NOTE: Does **NOT** free msu->data -- that must be freed manually
 *       with msu_data_free()
 * @param msu The MSU to be freed
 */
void destroy_msu(struct generic_msu *msu)
{
    if (msu->type->destroy)
        msu->type->destroy(msu);
    msu_free(msu);
}

/** Deserializes data received from remote MSU and enqueues the
 * message payload onto the msu queue.
 *
 * NOTE: If there are substructures in the buffer to be received,
 *       a type-specific deserialize function will have to be
 *       implemented.
 *
 * TODO: I don't think "void *buf" is necessary.
 * @param self MSU to receive data
 * @param msg remote message to be received, containing msg->payload
 * @param buf ???
 * @param bufsize ???
 * @return 0 on success, -1 on error
 */
int default_deserialize(struct generic_msu *self, intermsu_msg *msg,
                        void *buf, uint16_t bufsize){
    if (self){
        struct generic_msu_queue_item *recvd =  malloc(sizeof(*recvd));
        if (!(recvd))
            return -1;
        recvd->buffer_len = bufsize;
        recvd->buffer = malloc(bufsize);
        recvd->id = msg->data_id;
        if (!(recvd->buffer)){
            free(recvd);
            return -1;
        }
        memcpy(recvd->buffer, msg->payload, bufsize);
        generic_msu_queue_enqueue(&self->q_in, recvd);
        return 0;
    }
    return -1;
}

uint32_t default_generate_id(struct generic_msu *self,
                             struct generic_msu_queue_item *queue_item){
    int len = queue_item->buffer_len < 96 ? queue_item->buffer_len : 96;
    uint32_t id;
    HASH_VALUE(queue_item->buffer, len, id);
    return id;
}

/** Serializes the data of an msu_queue_item and sends it
 * to be deserialized by a remote msu.
 *
 * Copies data->buffer onto msg->payload. If something more
 * complicated has to be done, use a type-specific implementation
 * of this function
 *
 * Frees the message after enqueuing
 *
 * @param src MSU sending the message
 * @param data item to be enqueued onto next MSU
 * @param dst MSU destination to receive the message
 * @return -1 on error, >=0 on success
 */
int default_send_remote(struct generic_msu *src, struct generic_msu_queue_item *data,
                        struct msu_endpoint *dst){
    struct dedos_intermsu_message *msg = malloc(sizeof(*msg));
    if (!msg){
        log_error("Unable to allocate memory for intermsu msg%s", "");
        return -1;
    }

    msg->dst_msu_id = dst->id;
    msg->src_msu_id = src->id;
    msg->data_id = data->id;

    // TODO: Is this next line right? src->proto_number?
    msg->proto_msg_type = src->type->proto_number;
    msg->payload_len = data->buffer_len;
    msg->payload = malloc(msg->payload_len);
    if (!msg->payload){
        log_error("Unable to allocate memory for intermsu payload %s", "");
        free(msg);
        return -1;
    }

    memcpy(msg->payload, data->buffer, msg->payload_len);
    struct dedos_thread_msg *thread_msg = malloc(sizeof(*thread_msg));
    if (!thread_msg){
        log_error("Unable to allocate dedos_thread_msg%s", "");
        free(msg->payload);
        free(msg);
        return -1;
    }
    thread_msg->next = NULL;
    thread_msg->action = FORWARD_DATA;
    thread_msg->action_data = dst->ipv4;

    thread_msg->buffer_len = sizeof(*msg) + msg->payload_len;
    thread_msg->data = msg;

    /* add to allthreads[0] queue,since main thread is always at index 0 */
    /* need to create thread_msg struct with action = forward */
#ifdef DATAPLANE_PROFILING
    msg->payload_seq_count = data->dp_profile_info.dp_seq_count;
    msg->payload_request_id = data->dp_profile_info.dp_id;
    log_dp_event(-1, REMOTE_SEND, &data->dp_profile_info);
    log_debug("Copied item request id: %d",msg->payload_request_id);
#endif

    int rtn = dedos_thread_enqueue(main_thread, thread_msg);
    if (rtn < 0){
        log_error("Failed to enqueue data in main thread queue%s", "");
        free(thread_msg);
        free(msg->payload);
        free(msg);
        return -1;
    }

    log_debug("Successfully enqueued msg in main queue, size: %d", rtn);
    return rtn;
}

/**
 * Default function to enqueue data onto a local msu.
 *
 * I see no reason why this function wouldn't suffice for all msus,
 * but if more (or more complicated) data structures must be sent,
 * provide a custom implementation in the msu_type source.
 *
 * @param src MSU sending the data
 * @param data queue item to be sent
 * @param dst MSU receiving the data
 * @return 0 on success, -1 on error
 */
int default_send_local(struct generic_msu *src, struct generic_msu_queue_item *data,
                       struct msu_endpoint *dst){
    log_debug("Enqueuing locally to dst with id%d", dst->id);
    return generic_msu_queue_enqueue(dst->msu_queue, data);
}

struct round_robin_hh_key{
    int type_id;
    uint32_t ip_address;
};

/** Chooses the destination with the shortest queue length.
 * NOTE: Destination must be local in order to be chosen
 */
struct msu_endpoint *shortest_queue_route(struct msu_type *type, struct generic_msu *sender,
                                    struct generic_msu_queue_item *data) {
    struct route_set *type_set = get_type_from_route_set(&sender->routes, type->type_id);
    struct msu_endpoint *best_endpoint = get_shortest_queue_endpoint(type_set, data->id);
    if ( best_endpoint == NULL ){
        log_error("Cannot enqueue to shortest-length queue when all destinations are remote");
    }
    return best_endpoint;
}

struct msu_endpoint *default_routing(struct msu_type *type, struct generic_msu *sender,
                                     struct generic_msu_queue_item *data){
    struct route_set *type_set = get_type_from_route_set(&sender->routes, type->type_id);

    if (type_set == NULL){
        log_error("No routes available from msu %d to type %d",
                  sender->id, type->type_id);
        return NULL;
    }
    struct msu_endpoint *destination = get_route_endpoint(type_set, data->id);
    return destination;
}

/** Calls typical round_robin routing iteratively until it gets a destination
 * with the same ip address as specified.
 *
 * NOTE: As the queue item would contain ip addresses in a msu-type specific format,
 *       ip_address must be extracted by the destination msu before being passed into
 *       this function. Thus, code should look something like:
 *       ...
 *       type->route = type_do_routing;
 *       ...
 *       struct msu_endpoint type_do_routing(msu_type *type, struct generic_msu *sender,
 *                                           msu_queue_item *data){
 *           uint32_t ip_address = extract_ip_from_type_data(data);
 *           return round_robin_within_ip(type, sender, ip_address);
 *       }
 *
 * TODO: Soon to be handled by router object.
 *
 * @param type MSU type to be located
 * @param sender MSU sending the data
 * @param ip_address requested ip of destination MSU
 * @return msu_endpoint with requested IP, or NULL if N/A
 */
struct msu_endpoint *round_robin_within_ip(struct msu_type *type, struct generic_msu *sender,
                                           uint32_t ip_address){

    struct route_set *type_set = get_type_from_route_set(&sender->routes, type->type_id);
    int previous_index = (intptr_t)sender->routing_state;
    int new_index = previous_index++;

    int was_null = 0;

    struct msu_endpoint *dst;

    while ( 1 ) {
        dst = get_endpoint_by_index(type_set, new_index);
        if ( dst == NULL ){
            if (was_null){
                break;
            } else {
                was_null = 1;
                new_index = 0;
            }
        } else if ( dst->ipv4 == ip_address || (dst->locality == MSU_IS_LOCAL && ip_address == 0)) {
            break;
        } else {
            new_index++;
        }
    }

    if ( dst == NULL ){
        log_error("Could not find destination of type %d with correct ip (%d) from sender %d",
                type->type_id, ip_address, sender->id);
        return NULL;
    }

    sender->routing_state = (intptr_t)new_index;
    return dst;
}

/* Picks the next endpoing in a RR fashion while considering 4 tuple. This is to
* ensure that items for the same flow are sent to same destination
*
* TODO: Soon to be handled by router object.
*
* @param type MSU type to be located
* @param sender MSU sending the data
* @param source ip_address of data frame
* @param source port of data frame
* @param destination ip of data frame
* @param destination port of data frame
* @return msu_endpoint or NULL
*
struct msu_endpoint *round_robin_with_four_tuple(struct msu_type *type, struct generic_msu *sender,
                                    uint32_t src_addr, uint16_t src_port,
                                    uint32_t dst_addr, uint16_t dst_port){
    log_debug("Inside rr with 4 tup");
    log_debug("Received 4 tuple: %u:%u -> %u:%u",src_addr, src_port, dst_addr, dst_port);
    struct msu_endpoint *dst_msus =
        get_all_type_msus(sender->rt_table, type->type_id);

    struct msu_endpoint *dst = NULL;
    int count_next_msus = HASH_COUNT(dst_msus);
    log_debug("Available MSU for dst of type %s: %d",type->name, count_next_msus);

    //Only using one msu for redoing usenix tests.
    //update routing logic to support more handshake workers.
    dst = dst_msus;

    return dst;
}
*/

struct msu_endpoint *route_by_msu_id(struct msu_type *type, struct generic_msu *sender,
                                     int msu_id){
    struct route_set *type_set = get_type_from_route_set(&sender->routes, type->type_id);
    struct msu_endpoint *destination = get_endpoint_by_id(type_set, msu_id);

    return destination;
}

/**
 * Private function, sends data stored in a queue_item to a destination msu,
 * either local or remote.
 *
 * @param dst Destination MSU
 * @param src Source MSU
 * @param data Data to be enqueued on or sent to destination
 * @return -1 on error, 0 on success
 */
int send_to_dst(struct msu_endpoint *dst, struct generic_msu *src, struct generic_msu_queue_item *data){
    if (dst->locality == MSU_IS_LOCAL){
        if (!(dst->msu_queue)){
            log_error("Queue pointer not found%s", "");
            return -1;
        }
        int rtn = src->type->send_local(src, data, dst);
        if (rtn < 0){
            log_error("Failed to enqueue next msu%s", "");
            return -1;
        }
        return 0;
    } else if (dst->locality == MSU_IS_REMOTE){
        int rtn = src->type->send_remote(src, data, dst);
#ifdef DATAPLANE_PROFILING
        //copy queue item profile log to in memory log before its freed
        //copy_queue_item_dp_data(&data->dp_profile_info);
        int i = 0;
        pthread_mutex_lock(&fp_log_mutex);
        for(i = 0; i < data->dp_profile_info.dp_entry_count; i++){
            fprintf(fp_log, "%s\n",data->dp_profile_info.dp_log_entries[i]);
        }
        pthread_mutex_unlock(&fp_log_mutex);
#endif
        if (rtn < 0){
            log_error("Failed to send to remote runtime%s", "");
        }
        if (data){
            log_debug("Freeing data buffer and data because of remote send");
            free(data->buffer);
            free(data);
        }
        return rtn;
    } else {
        log_error("Unknown locality: %d", dst->locality);
        return -1;
    }
}

int msu_route(struct msu_type *type, struct generic_msu *sender,
                struct generic_msu_queue_item *data){
    if (sender->type->generate_id == NULL){
        if (data->id == 0){
            log_warn("Data ID not assigned, and sender %d (%s) cannot assign ID",
                     sender->id, sender->type->name);
        }
    } else {
        if (data->id != 0){
            log_warn("Data ID being reassigned from %d by msu %d",data->id, sender->id);
        }
        data->id = sender->type->generate_id(sender, data);
        log_debug("Assigned queue item %p id %d", data, data->id);
    }

    struct msu_endpoint *dst = type->route(type, sender, data);
    if (dst == NULL){
        log_error("No destination endpoint of type %s for msu %d",
                  type->name, sender->id);
        return -1;
    }

    int rtn = send_to_dst(dst, sender, data);
    if (rtn < 0){
        log_error("Error sending to destination msu");
        if (data){
            free(data->buffer);
            free(data);
        }
    }
    return rtn;
}


/** Receives and handles dequeued data from another MSU.
 * Also handles sending message to the next MSU, if applicable.
 * Calls the receieve function of the provided MSU type if available.
 * @param self MSU receiving data
 * @param data received data
 * @return 0 on success, -1 on error
 */
int msu_receive(struct generic_msu *self, struct generic_msu_queue_item *data){

    // Check that all of the relevant structures exist
    if (! (self && self->type->receive) ){
        if (data){
            free(data->buffer);
            free(data);
        }
        log_error("Could not receive data%s", "");
        return -1;
    }

    // Receieve the data, get the type of the next msu to send to
    aggregate_start_time(MSU_INTERNAL_TIME, self->id);
    int type_id = self->type->receive(self, data);
    aggregate_end_time(MSU_INTERNAL_TIME, self->id);
    if (type_id <= 0) {
        // If type_id <= 0 it can either be an error (< 0) or just
        // not have a subsequent destination
        if (type_id == -10){ // Special case for tcp related msus, return this
            //no need to free data pointer, if was set, the receive function
            //takes care of freeing the buffer
            return 0;
        }
        if (type_id < 0){
            log_warn("MSU %d returned error code %d", self->id, type_id);
        }
        if (data){
            if (data->buffer) {
                free(data->buffer);
            }
            free(data);
        }
        return type_id;
    }
    log_debug("type ID of next MSU is %d", type_id);

    // Get the MSU type to deliver to
    struct msu_type *type = msu_type_by_id((unsigned int)type_id);
    if (type == NULL) {
        log_error("Type ID %d not recognized", type_id);
        if (data) {
            free(data->buffer);
            free(data);
        }
        return -1;
    }

    // Get the specific MSU to deliver to
    struct msu_endpoint *dst = type->route(type, self, data);
    if (dst == NULL) {
        log_error("No destination endpoint of type %s (%d) for msu %d",
                  type->name, type_id, self->id);
        if (data) {
            free(data->buffer);
            free(data);
        }
        return -1;
    }
    log_debug("Next msu id is %d", dst->id);

    // Send to the specific destination
    int rtn = msu_route(type, self, data);
    if (rtn < 0){
        log_error("Error sending from msu %d", self->id);
        if (data){
            free(data->buffer);
            free(data);
        }
    }

    return rtn;
}
