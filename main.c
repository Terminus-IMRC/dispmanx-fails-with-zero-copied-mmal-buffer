/*
 * Copyright (c) 2017 Sugizaki Yukimasa (ysugi@idein.jp)
 * All rights reserved.
 *
 * This software is licensed under a Modified (3-Clause) BSD License.
 * You should have received a copy of this license along with this
 * software. If not, contact the copyright holder above.
 */

#include <interface/mmal/mmal.h>
#include <interface/mmal/mmal_logging.h>
#include <interface/mmal/util/mmal_util.h>
#include <interface/mmal/util/mmal_util_params.h>
#include <interface/mmal/util/mmal_default_components.h>
#include <bcm_host.h>
#include <stdio.h>
#include <stdlib.h>


#define COMPONENT "vc.ril.source"
#define ZERO_COPY 0
#define ENCODING MMAL_ENCODING_RGBA
#define WIDTH  128
#define HEIGHT 128


#define _assert(x) \
    do { \
        int ret = (int) (x); \
        if (!ret) { \
            fprintf(stderr, "%s:%d: Assertation failed: %s\n", __FILE__, __LINE__, #x); \
            exit(EXIT_FAILURE); \
        }\
    } while (0)

#define _check_vcos(x) \
    do { \
        VCOS_STATUS_T status = (x); \
        if (status != VCOS_SUCCESS) { \
            fprintf(stderr, "%s:%d: VCOS call failed: 0x%08x\n", __FILE__, __LINE__, status); \
            exit(EXIT_FAILURE); \
        } \
    } while (0)

#define _check_dispmanx(x) \
    do { \
        int ret = (int) (x); \
        if (ret) { \
            fprintf(stderr, "%s:%d: Dispmanx call failed: 0x%08x\n", __FILE__, __LINE__, ret); \
            exit(EXIT_FAILURE); \
        } \
    } while (0)


#define _check_mmal(x) \
    do { \
        MMAL_STATUS_T status = (x); \
        if (status != MMAL_SUCCESS) { \
            fprintf(stderr, "%s:%d: MMAL call failed: 0x%08x\n", __FILE__, __LINE__, status); \
            exit(EXIT_FAILURE); \
        } \
    } while (0)


static void show_image_dispmanx(void *image)
{
    VC_RECT_T rect, src_rect, dst_rect;
    DISPMANX_DISPLAY_HANDLE_T display;
    DISPMANX_UPDATE_HANDLE_T update;
    DISPMANX_RESOURCE_HANDLE_T resource;
    DISPMANX_ELEMENT_HANDLE_T element;
    VC_DISPMANX_ALPHA_T alpha = {
        .flags = DISPMANX_FLAGS_ALPHA_FIXED_ALL_PIXELS,
        .opacity = 128,
        .mask = 0
    };
    unsigned vc_image_ptr;

    _assert(display = vc_dispmanx_display_open(0));
    _assert(update = vc_dispmanx_update_start(0));
    _assert(resource = vc_dispmanx_resource_create(VC_IMAGE_RGBA32, WIDTH, HEIGHT, &vc_image_ptr));
    _check_dispmanx(vc_dispmanx_rect_set(&rect, 0, 0, WIDTH, HEIGHT));

    /* xxx: Fail with zero-copied image from MMAL? */
    _check_dispmanx(vc_dispmanx_resource_write_data(resource, VC_IMAGE_RGBA32, ALIGN_UP(WIDTH * 4, 32), image, &rect));

    _check_dispmanx(vc_dispmanx_rect_set(&src_rect, 0, 0, WIDTH << 16, HEIGHT << 16));
    _check_dispmanx(vc_dispmanx_rect_set(&dst_rect, 0, 0, WIDTH, HEIGHT));
    _assert(element = vc_dispmanx_element_add(
                update, display,
                5,
                &dst_rect,
                resource,
                &src_rect,
                DISPMANX_PROTECTION_NONE,
                &alpha, NULL, VC_IMAGE_ROT0));
    _check_dispmanx(vc_dispmanx_resource_delete(resource));
    _check_dispmanx(vc_dispmanx_update_submit_sync(update));

    sleep(1);

    _assert(update = vc_dispmanx_update_start(0));
    _check_dispmanx(vc_dispmanx_element_remove(update, element));
    _check_dispmanx(vc_dispmanx_update_submit_sync(update));
    _check_dispmanx(vc_dispmanx_display_close(display));
}

static VCOS_SEMAPHORE_T sem;

static void callback_camera_output(MMAL_PORT_T *port, MMAL_BUFFER_HEADER_T *buffer)
{
    (void) port;
    fprintf(stderr, "Buffer %p %p, filled %d, flags 0x%08x\n", buffer, buffer->data, buffer->length, buffer->flags);
    show_image_dispmanx(buffer->data);
    _check_vcos(vcos_semaphore_post(&sem));
}

int main()
{
    unsigned i;
    MMAL_COMPONENT_T *component = NULL;
    MMAL_POOL_T *pool_camera = NULL;

    bcm_host_init();
    _check_vcos(vcos_semaphore_create(&sem, "example", 1));

    _check_mmal(mmal_component_create(COMPONENT, &component));
    {
        MMAL_PORT_T *output = component->output[0];
        MMAL_POOL_T *pool = NULL;
        unsigned num;
        MMAL_PARAMETER_VIDEO_SOURCE_PATTERN_T source_pattern = {
            .hdr = {
                .id = MMAL_PARAMETER_VIDEO_SOURCE_PATTERN,
                .size = sizeof(source_pattern)
            },
            .pattern = MMAL_VIDEO_SOURCE_PATTERN_RANDOM
        };

        output->format->encoding = ENCODING;
        output->format->es->video.width  = VCOS_ALIGN_UP(WIDTH,  32);
        output->format->es->video.height = VCOS_ALIGN_UP(HEIGHT, 16);
        output->format->es->video.crop.x = 0;
        output->format->es->video.crop.y = 0;
        output->format->es->video.crop.width  = WIDTH;
        output->format->es->video.crop.height = HEIGHT;
        _check_mmal(mmal_port_format_commit(output));
        _check_mmal(mmal_port_parameter_set(output, &source_pattern.hdr));
        _check_mmal(mmal_port_parameter_set_boolean(output, MMAL_PARAMETER_ZERO_COPY, ZERO_COPY));

        pool = pool_camera = mmal_port_pool_create(output, output->buffer_num, output->buffer_size);
        _assert(pool != NULL);

        _check_mmal(mmal_port_enable(output, callback_camera_output));
        num = mmal_queue_length(pool->queue);
        for (i = 0; i < num; i ++) {
            MMAL_BUFFER_HEADER_T *header = mmal_queue_get(pool->queue);
            _assert(header != NULL);
            _check_mmal(mmal_port_send_buffer(output, header));
            fprintf(stderr, "Sent header %p %p %d 0x%08x\n", header, header->data, header->length, header->flags);
        }
    }

    vcos_semaphore_wait(&sem);

    _check_mmal(mmal_port_disable(component->output[0]));
    mmal_port_pool_destroy(component->output[0], pool_camera);
    _check_mmal(mmal_component_destroy(component));
    vcos_semaphore_delete(&sem);
    bcm_host_deinit();
    return 0;
}
