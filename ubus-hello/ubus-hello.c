#include <stdio.h>

#include <libubus.h>
#include <libubox/blobmsg.h>
#include <libubox/uloop.h>

static int say(struct ubus_context *ctx, struct ubus_object *obj,
               struct ubus_request_data *req, const char *method,
               struct blob_attr *msg)
{
    struct blob_buf reply = {0};
    int result;

    (void)obj;
    (void)method;
    (void)msg;

    if (blob_buf_init(&reply, 0) != 0)
        return UBUS_STATUS_UNKNOWN_ERROR;

    if (blobmsg_add_string(&reply, "message", "Hello World") != 0) {
        blob_buf_free(&reply);
        return UBUS_STATUS_UNKNOWN_ERROR;
    }

    result = ubus_send_reply(ctx, req, reply.head);
    blob_buf_free(&reply);
    return result;
}

static const struct ubus_method methods[] = {
    UBUS_METHOD_NOARG("say", say),
};

static struct ubus_object_type object_type =
    UBUS_OBJECT_TYPE("hello-demo", methods);

static struct ubus_object object = {
    .name = "example.hello",
    .type = &object_type,
    .methods = methods,
    .n_methods = ARRAY_SIZE(methods),
};

int main(void)
{
    struct ubus_context *ctx;
    int result;

    if (uloop_init() != 0) {
        fputs("uloop_init failed\n", stderr);
        return 1;
    }

    ctx = ubus_connect(NULL);
    if (!ctx) {
        fputs("ubus_connect failed\n", stderr);
        uloop_done();
        return 1;
    }

    ubus_add_uloop(ctx);
    result = ubus_add_object(ctx, &object);
    if (result != 0) {
        fprintf(stderr, "ubus_add_object: %s\n", ubus_strerror(result));
        ubus_free(ctx);
        uloop_done();
        return 1;
    }

    fputs("Registered example.hello\n", stderr);
    uloop_run();

    ubus_remove_object(ctx, &object);
    ubus_free(ctx);
    uloop_done();
    return 0;
}
