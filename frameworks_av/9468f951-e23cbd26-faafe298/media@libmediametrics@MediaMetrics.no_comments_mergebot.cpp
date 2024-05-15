#define LOG_TAG "MediaMetrics"
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <media/MediaAnalyticsItem.h>
#include <media/MediaMetricsItem.h>
#include <media/MediaMetrics.h>
using namespace android::mediametrics;
mediametrics_handle_t mediametrics_create(mediametricskey_t key) {
  Item *item = Item::create(key);
  return (mediametrics_handle_t)item;
}
void mediametrics_delete(mediametrics_handle_t handle) {
  Item *item = (Item *)handle;
  if (item == NULL) return;
  delete item;
}
mediametricskey_t mediametrics_getKey(mediametrics_handle_t handle) {
  Item *item = (Item *)handle;
  if (item == NULL) return NULL;
  return strdup(item->getKey().c_str());
}
void mediametrics_setUid(mediametrics_handle_t handle, uid_t uid) {
  Item *item = (Item *)handle;
  if (item != NULL) item->setUid(uid);
}
void mediametrics_setInt32(mediametrics_handle_t handle, attr_t attr,
                           int32_t value) {
  Item *item = (Item *)handle;
  if (item != NULL) item->setInt32(attr, value);
}
void mediametrics_setInt64(mediametrics_handle_t handle, attr_t attr,
                           int64_t value) {
  Item *item = (Item *)handle;
  if (item != NULL) item->setInt64(attr, value);
}
void mediametrics_setDouble(mediametrics_handle_t handle, attr_t attr,
                            double value) {
  Item *item = (Item *)handle;
  if (item != NULL) item->setDouble(attr, value);
}
void mediametrics_setRate(mediametrics_handle_t handle, attr_t attr,
                          int64_t count, int64_t duration) {
  Item *item = (Item *)handle;
  if (item != NULL) item->setRate(attr, count, duration);
}
void mediametrics_setCString(mediametrics_handle_t handle, attr_t attr,
                             const char *value) {
  Item *item = (Item *)handle;
  if (item != NULL) item->setCString(attr, value);
}
void mediametrics_addInt32(mediametrics_handle_t handle, attr_t attr,
                           int32_t value) {
  Item *item = (Item *)handle;
  if (item != NULL) item->addInt32(attr, value);
}
void mediametrics_addInt64(mediametrics_handle_t handle, attr_t attr,
                           int64_t value) {
  Item *item = (Item *)handle;
  if (item != NULL) item->addInt64(attr, value);
}
void mediametrics_addDouble(mediametrics_handle_t handle, attr_t attr,
                            double value) {
  Item *item = (Item *)handle;
  if (item != NULL) item->addDouble(attr, value);
}
void mediametrics_addRate(mediametrics_handle_t handle, attr_t attr,
                          int64_t count, int64_t duration) {
  Item *item = (Item *)handle;
  if (item != NULL) item->addRate(attr, count, duration);
}
bool mediametrics_getInt32(mediametrics_handle_t handle, attr_t attr,
                           int32_t *value) {
  Item *item = (Item *)handle;
  if (item == NULL) return false;
  return item->getInt32(attr, value);
}
bool mediametrics_getInt64(mediametrics_handle_t handle, attr_t attr,
                           int64_t *value) {
  Item *item = (Item *)handle;
  if (item == NULL) return false;
  return item->getInt64(attr, value);
}
bool mediametrics_getDouble(mediametrics_handle_t handle, attr_t attr,
                            double *value) {
  Item *item = (Item *)handle;
  if (item == NULL) return false;
  return item->getDouble(attr, value);
}
bool mediametrics_getRate(mediametrics_handle_t handle, attr_t attr,
                          int64_t *count, int64_t *duration, double *rate) {
  Item *item = (Item *)handle;
  if (item == NULL) return false;
  return item->getRate(attr, count, duration, rate);
}
bool mediametrics_getCString(mediametrics_handle_t handle, attr_t attr,
                             char **value) {
  Item *item = (Item *)handle;
  if (item == NULL) return false;
  return item->getCString(attr, value);
}
void mediametrics_freeCString(char *value) { free(value); }
bool mediametrics_selfRecord(mediametrics_handle_t handle) {
  Item *item = (Item *)handle;
  if (item == NULL) return false;
  return item->selfrecord();
}
mediametrics_handle_t mediametrics_dup(mediametrics_handle_t handle) {
  android::MediaAnalyticsItem *item = (android::MediaAnalyticsItem *)handle;
  if (item == NULL) return android::MediaAnalyticsItem::convert(item);
  return android::MediaAnalyticsItem::convert(item->dup());
}
const char *mediametrics_readable(mediametrics_handle_t handle) {
  Item *item = (Item *)handle;
  if (item == NULL) return "";
  return item->toCString();
}
int32_t mediametrics_count(mediametrics_handle_t handle) {
  Item *item = (Item *)handle;
  if (item == NULL) return 0;
  return item->count();
}
bool mediametrics_isEnabled() {
  return Item::isEnabled();
}
bool mediametrics_getAttributes(mediametrics_handle_t handle, char **buffer,
                                size_t *length) {
  Item *item = (Item *)handle;
  if (item == NULL) return false;
  return item->writeToByteString(buffer, length) == android::NO_ERROR;
}
