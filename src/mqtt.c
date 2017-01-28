#include <pthread.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdio.h>
#include <mosquitto.h>
#include "helper.h"
#include "options.h"
#include "dbg.h"
#include <time.h>
#include <message.h>
#include "mqtt_utils.h"
#include <compiler.h>
#include <json-c/json.h>
#include "notify.h"

#define M 10
#define N 100

struct mosquitto *mosq = NULL;
bool connected = false;
time_t m0=0;
char mqtt_id[27];

int dial_mqtt();

void read_message()
{

}

void my_connect_callback(struct mosquitto *mosq, UNUSED(void *userdata), int result)
{
  // Why?
  if(result) {
    return;
  }

  // On connect, send a message to the MQTT broker
  connected = true;

  int k,n;
  k = strlen(options.key);
  n = strlen(options.topic);
  char topic[k+n+19];
  topic_id_generate(topic, options.topic, options.key);

  options.qos = 1;
  char t[128];
  strcat(t, options.topic);

  mosquitto_subscribe(mosq, NULL, topic, options.qos);

  if (strcmp(options.topic, "") == 0) {
    return;
  }

  json_object *jobj = json_object_new_object();
  json_object *jmeta = json_object_new_object();

  // refactor
  json_object_object_add(jobj, "app", json_object_new_string("socketman"));
  json_object_object_add(jobj, "timestamp", json_object_new_int(time(NULL)));
  json_object_object_add(jobj, "event_type", json_object_new_string("CONNECT"));
  json_object_object_add(jmeta, "online", json_object_new_string("1"));
  json_object_object_add(jmeta, "client_id", json_object_new_string(mqtt_id));
  json_object_object_add(jobj, "meta", jmeta);

  const char *resp = json_object_to_json_string(jobj);

  // refactor and de-dup
  char topic_a[128];

  // Status at the front is just for status / online / offline updates
  strcpy(topic_a, "status/");
  strcat(topic_a, options.topic);
  strcat(topic_a, "/");
  strcat(topic_a, options.key);
  strcat(topic_a, "/");
  strcat(topic_a, options.mac);

  mosquitto_publish(mosq, 0, topic_a, strlen(resp), resp, 1, false);
  json_object_put(jobj);
}

static char *rand_string(char *str, char *prefix, size_t size)
{
  const char charset[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJK0123456789...";
  if (size) {
    strcpy(str, prefix);
    --size;
    int im, n;
    for (n = strlen(prefix); n < N && im < M; ++n) {
      int key = rand() % (int) (sizeof charset - 1);
      str[n] = charset[key];
    }
    str[size] = '\0';
  }
  return str;
}

// Refactor whole function
void my_message_callback(struct mosquitto *mosq, UNUSED(void *userdata), const struct mosquitto_message *message)
{
  if (!message->payloadlen) {
    debug("Error decoding JSON, not processing payload.");
    return;
  }

  time_t now = time(NULL);
  debug("Message received at %lld", (long long)now);

  if (!message->payloadlen) {
    return;
  }

  char type[10];
  char mid[36+1];

  char cmd[strlen(message->payload)+1];

  mid[0] = '\0';
  cmd[0] = '\0';

  // Unmarshalls the payload into logical parts
  process_message((const char*)message->payload, cmd, mid, type, strlen(message->payload)+1);

  // Runs special commands, based on the type of request
  run_special(type);

  // If payload missing, do nothing!
  if (cmd[0] == '\0') {
    debug("Payload missing CMD or ID, exiting.");
    return;
  }

  // Save output to file if debug flag is set
  if (options.debug)
    save_config("/tmp/.configs", cmd);

  // Refactor
  char delivery[117];
  strcpy(delivery, "delivery/");
  strcat(delivery, options.topic);
  strcat(delivery, "/");
  strcat(delivery, options.key);
  strcat(delivery, "/");
  strcat(delivery, options.mac);

  char *suffix = NULL;
  if (mid[0] == '\0') {
    suffix = "/messages";
    strcat(delivery, suffix);
    rand_string(mid, "sm_", 44);
    debug("Missing ID. Auto-generating: %s. Topic: %s", mid, delivery);
  }
  // Needs check if running, check time, check live cmd.

  json_object *jobj = json_object_new_object();
  json_object *jmeta = json_object_new_object();

  // Refactor
  json_object_object_add(jobj, "id", json_object_new_string(mid));
  json_object_object_add(jobj, "app", json_object_new_string("socketman"));
  json_object_object_add(jobj, "timestamp", json_object_new_int(time(NULL)));
  json_object_object_add(jobj, "event_type", json_object_new_string("DELIVERED"));
  json_object_object_add(jmeta, "delivered", json_object_new_boolean(1));
  json_object_object_add(jobj, "meta", jmeta);
  const char *report = json_object_to_json_string(jobj);

  mosquitto_publish(mosq, 0, delivery, strlen(report), report, 1, false);

  // Message processing
  FILE *fp;
  int response = -1;
  char buffer[51200];
  buffer[0] = '\0';

  fp = popen(cmd, "r");
  if (fp != NULL) {
    response = 0;
    memset(buffer, '\0', sizeof(buffer));
    fread(buffer, sizeof(char), 51200, fp);
    pclose(fp);
  }

  if (options.debug == 1)
    debug("%s", buffer);

  if (strlen(buffer) == 0) {
    strcpy(buffer, "DNE");
  }

  // Will publish the job back to CT via REST API
  if (options.rest) {
    cmd_notify(response, mid, buffer);
    return;
  }

  // Refactor
  // Else, let's publish back
  jobj = json_object_new_object();
  json_object_object_add(jobj, "id", json_object_new_string(mid));
  json_object_object_add(jobj, "app", json_object_new_string("socketman"));
  json_object_object_add(jobj, "timestamp", json_object_new_int(time(NULL)));
  json_object_object_add(jobj, "event_type", json_object_new_string("PROCESSED"));
  json_object_object_add(jmeta, "msg", json_object_new_string(buffer));
  // Should include a flag for the status of the job, maybe it fails.
  json_object_object_add(jobj, "meta", jmeta);
  report = json_object_to_json_string(jobj);

  char pub[112];
  strcpy(pub, "pub/");
  strcat(pub, options.topic);
  strcat(pub, "/");
  strcat(pub, options.key);
  strcat(pub, "/");
  strcat(pub, options.mac);

  if (suffix != NULL) {
    strcat(pub, suffix);
  }

  mosquitto_publish(mosq, 0, pub, strlen(report), report, 1, false);
  json_object_put(jobj);
  debug("Message published to %s!", pub);

  return;
}

void my_subscribe_callback(UNUSED(struct mosquitto *mosq), UNUSED(void *userdata), int mid, int qos_count, const int *granted_qos)
{
  int i;
  debug("Subscribed (mid: %d): %d", mid, granted_qos[0]);
  for(i=1; i<qos_count; i++) {
    debug("QOS: %d", granted_qos[i]);
  }
}

void *reconnect(UNUSED(void *x))
{
  while(!connected) {
    debug("Unable to connect to %s. Sleeping 15", options.mqtt_host);
    sleep(5);
    // Swap port, test connection //
    if (!dial_mqtt()) {
      debug("Reconnected to %s, yay!", options.mqtt_host);
      break;
    }
  }
  return NULL;
}

void my_disconnect_callback(UNUSED(struct mosquitto *mosq), UNUSED(void *userdata), UNUSED(int rc))
{
  connected = false;
  debug("Lost connection with broker: %s", options.mqtt_host);
  sleep(10);
}

void mqtt_connect() {
  if (strcmp(options.mqtt_host, "") == 0) {
    debug("No MQTT host, skipping connect.");
    return;
  }

  // Do the port thing here SM....
  // check connectivity
  // if online, change port
  // Notify tony of port change

  /* int ports[4] = {0,0,0,0}; */
  /* int test[4] = {80,443,8080,8443}; */

  int rc = dial_mqtt();

  if (!rc) {
    return;
  }

  pthread_t conn_thread;
  if(pthread_create(&conn_thread, NULL, reconnect, NULL)) {
    fprintf(stderr, "Error creating thread\n");
    return;
  }

  return;
}

void ping_mqtt()
{
  debug("Sending MQTT ping!");

  int k,n;
  k = strlen(options.key);
  n = strlen(options.topic);
  char topic[k+n+19];
  topic_id_generate(topic, options.topic, options.key);

  if (strcmp(options.topic, "") == 0) {
    return;
  }

  json_object *jobj = json_object_new_object();

  json_object_object_add(jobj, "timestamp", json_object_new_int(time(NULL)));
  json_object_object_add(jobj, "app", json_object_new_string("socketman"));
  json_object_object_add(jobj, "event_type", json_object_new_string("PING"));
  const char *resp = json_object_to_json_string(jobj);

  char topic_a[128];

  strcpy(topic_a, "status/");
  strcat(topic_a, options.topic);
  strcat(topic_a, "/");
  strcat(topic_a, options.key);
  strcat(topic_a, "/");
  strcat(topic_a, options.mac);

  mosquitto_publish(mosq, 0, topic_a, strlen(resp), resp, 1, false);
  json_object_put(jobj);
}

int dial_mqtt()
{
  mosquitto_lib_init();
  debug("Connecting to broker...");

  client_id_generate(mqtt_id);

  int keepalive = 30;
  bool clean_session = true;

  mosq = mosquitto_new(mqtt_id, clean_session, NULL);
  if(!mosq){
    switch(errno){
      case ENOMEM:
        fprintf(stderr, "Error: Out of memory.\n");
        break;
      case EINVAL:
        fprintf(stderr, "Error: Invalid id and/or clean_session.\n");
        break;
    }
    mosquitto_lib_cleanup();
    return 1;
  }

  if (options.tls == true) {
    debug("Connecting via encrypted channel");
    if (strcmp(options.cacrt, "") == 0) {
      debug("[ERROR] Missing ca file");
    }
    mosquitto_tls_opts_set(mosq, 1, NULL, NULL);
    mosquitto_tls_set(mosq, options.cacrt, NULL, NULL, NULL, NULL);
  }

  mosquitto_connect_callback_set(mosq, my_connect_callback);
  mosquitto_message_callback_set(mosq, my_message_callback);
  mosquitto_subscribe_callback_set(mosq, my_subscribe_callback);
  mosquitto_disconnect_callback_set(mosq, my_disconnect_callback);
  mosquitto_username_pw_set(mosq, options.username, options.password);

  if (strcmp(options.topic, "") != 0) {

    json_object *jobj = json_object_new_object();
    json_object *jmeta = json_object_new_object();

    json_object_object_add(jobj, "app", json_object_new_string("socketman"));
    json_object_object_add(jobj, "timestamp", json_object_new_int(time(NULL)));
    json_object_object_add(jobj, "event_type", json_object_new_string("CONNECT"));
    json_object_object_add(jmeta, "online", json_object_new_string("0"));
    /* json_object_object_add(jmeta, "msg", json_object_new_string("Went offline")); */
    json_object_object_add(jmeta, "client_id", json_object_new_string(mqtt_id));
    json_object_object_add(jobj, "meta", jmeta);

    const char *report = json_object_to_json_string(jobj);

    // Refactor, de-dup after testing
    char topic[128];
    strcpy(topic, "status/");
    strcat(topic, options.topic);
    strcat(topic, "/");
    strcat(topic, options.key);
    strcat(topic, "/");
    strcat(topic, options.mac);

    // Set the last will on the broker
    mosquitto_will_set(mosq, topic, strlen(report), report, 1, false);
    json_object_put(jobj);
  }

  int rc = mosquitto_connect_async(mosq, options.mqtt_host, options.port, keepalive);
  if (rc) {
    mosquitto_destroy(mosq);
    mosquitto_lib_cleanup();
    return(rc);
  }

  rc = mosquitto_loop_start(mosq);

  if(rc == MOSQ_ERR_NO_CONN){
    rc = 0;
    connected = 1;
  }

  if(rc){
    fprintf(stderr, "Error: %s\n", mosquitto_strerror(rc));
  }

  return rc;
}
