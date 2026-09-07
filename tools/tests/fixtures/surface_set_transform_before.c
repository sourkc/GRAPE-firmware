esp_err_t grape_surface_set_transform(grape_surface_t *surface, const grape_transform_t *transform)
{
    if (!surface || !transform || !surface_transform_finite(transform) || fabsf(transform->scale_x) < FLT_EPSILON || fabsf(transform->scale_y) < FLT_EPSILON) {
        return ESP_ERR_INVALID_ARG;
    }

    const grape_transform_t *old = &surface->transform;
    if (old->x == transform->x && old->y == transform->y &&
        old->scale_x == transform->scale_x && old->scale_y == transform->scale_y &&
        old->rotation == transform->rotation &&
        old->origin_x == transform->origin_x && old->origin_y == transform->origin_y) {
        return ESP_OK;
    }
    GRAPE_TIME_SCOPE(SURFACE_TRANSFORM);

    esp_err_t ret = mark_surface_coverage(surface);
    if (ret == ESP_OK) {
        surface->transform = *transform;
        grape_surface_recache(surface);
        ret = mark_surface_coverage(surface);
    }

    return ret;
}
