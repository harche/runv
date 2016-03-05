#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

#include "list.h"
#include "parse.h"

/* parse_utf_16() and process_string() are copied from https://github.com/kgabis/parson */
#include <ctype.h>

#define JSONFailure (-1)
#define JSONSuccess (0)
#define parson_malloc malloc
#define parson_free free

static int is_utf16_hex(const unsigned char *s) {
    return isxdigit(s[0]) && isxdigit(s[1]) && isxdigit(s[2]) && isxdigit(s[3]);
}

static int parse_utf_16(const char **unprocessed, char **processed) {
    unsigned int cp, lead, trail;
    char *processed_ptr = *processed;
    const char *unprocessed_ptr = *unprocessed;
    unprocessed_ptr++; /* skips u */
    if (!is_utf16_hex((const unsigned char*)unprocessed_ptr) || sscanf(unprocessed_ptr, "%4x", &cp) == EOF)
        return JSONFailure;
    if (cp < 0x80) {
        *processed_ptr = cp; /* 0xxxxxxx */
    } else if (cp < 0x800) {
        *processed_ptr++ = ((cp >> 6) & 0x1F) | 0xC0; /* 110xxxxx */
        *processed_ptr   = ((cp     ) & 0x3F) | 0x80; /* 10xxxxxx */
    } else if (cp < 0xD800 || cp > 0xDFFF) {
        *processed_ptr++ = ((cp >> 12) & 0x0F) | 0xE0; /* 1110xxxx */
        *processed_ptr++ = ((cp >> 6)  & 0x3F) | 0x80; /* 10xxxxxx */
        *processed_ptr   = ((cp     )  & 0x3F) | 0x80; /* 10xxxxxx */
    } else if (cp >= 0xD800 && cp <= 0xDBFF) { /* lead surrogate (0xD800..0xDBFF) */
        lead = cp;
        unprocessed_ptr += 4; /* should always be within the buffer, otherwise previous sscanf would fail */
        if (*unprocessed_ptr++ != '\\' || *unprocessed_ptr++ != 'u' || /* starts with \u? */
            !is_utf16_hex((const unsigned char*)unprocessed_ptr)          ||
            sscanf(unprocessed_ptr, "%4x", &trail) == EOF           ||
            trail < 0xDC00 || trail > 0xDFFF) { /* valid trail surrogate? (0xDC00..0xDFFF) */
            return JSONFailure;
        }
        cp = ((((lead-0xD800)&0x3FF)<<10)|((trail-0xDC00)&0x3FF))+0x010000;
        *processed_ptr++ = (((cp >> 18) & 0x07) | 0xF0); /* 11110xxx */
        *processed_ptr++ = (((cp >> 12) & 0x3F) | 0x80); /* 10xxxxxx */
        *processed_ptr++ = (((cp >> 6)  & 0x3F) | 0x80); /* 10xxxxxx */
        *processed_ptr   = (((cp     )  & 0x3F) | 0x80); /* 10xxxxxx */
    } else { /* trail surrogate before lead surrogate */
        return JSONFailure;
    }
    unprocessed_ptr += 3;
    *processed = processed_ptr;
    *unprocessed = unprocessed_ptr;
    return JSONSuccess;
}


/* Copies and processes passed string up to supplied length.
 Example: "\u006Corem ipsum" -> lorem ipsum */
static char* process_string(const char *input, size_t len) {
    const char *input_ptr = input;
    size_t initial_size = (len + 1) * sizeof(char);
    //size_t final_size = 0;
    char *output = (char*)parson_malloc(initial_size);
    char *output_ptr = output;
    //char *resized_output = NULL;
    while ((*input_ptr != '\0') && (size_t)(input_ptr - input) < len) {
        if (*input_ptr == '\\') {
            input_ptr++;
            switch (*input_ptr) {
                case '\"': *output_ptr = '\"'; break;
                case '\\': *output_ptr = '\\'; break;
                case '/':  *output_ptr = '/';  break;
                case 'b':  *output_ptr = '\b'; break;
                case 'f':  *output_ptr = '\f'; break;
                case 'n':  *output_ptr = '\n'; break;
                case 'r':  *output_ptr = '\r'; break;
                case 't':  *output_ptr = '\t'; break;
                case 'u':
                    if (parse_utf_16(&input_ptr, &output_ptr) == JSONFailure)
                        goto error;
                    break;
                default:
                    goto error;
            }
        } else if ((unsigned char)*input_ptr < 0x20) {
            goto error; /* 0x00-0x19 are invalid characters for json string (http://www.ietf.org/rfc/rfc4627.txt) */
        } else {
            *output_ptr = *input_ptr;
        }
        output_ptr++;
        input_ptr++;
    }
    *output_ptr = '\0';
    /* resize to new length */
    //final_size = (size_t)(output_ptr-output) + 1;
    //resized_output = (char*)parson_malloc(final_size);
    //if (resized_output == NULL)
    //    goto error;
    //memcpy(resized_output, output, final_size);
    //parson_free(output);
    //return resized_output;
    return output;
error:
    parson_free(output);
    return NULL;
}

char *json_token_str(char *js, jsmntok_t *t)
{
    return process_string(js+t->start, t->end - t->start);
}

int json_token_int(char *js, jsmntok_t *t)
{
	return strtol(json_token_str(js, t), 0, 10);
}

uint64_t json_token_ll(char *js, jsmntok_t *t)
{
	return strtoll(json_token_str(js, t), 0, 10);
}

int json_token_streq(char *js, jsmntok_t *t, char *s)
{
	return (strncmp(js + t->start, s, t->end - t->start) == 0 &&
		strlen(s) == (size_t)(t->end - t->start));
}

static int container_parse_cmd(struct hyper_container *c, char *json, jsmntok_t *toks)
{
	int i = 0, j;

	if (toks[i].type != JSMN_ARRAY) {
		fprintf(stdout, "cmd need array");
		return -1;
	}

	c->exec.argv = calloc(toks[i].size + 1, sizeof(*c->exec.argv));
	if (c->exec.argv == NULL) {
		fprintf(stderr, "allocate memory for exec argv failed\n");
		return -1;
	}

	c->exec.argv[c->exec.argc] = NULL;
	c->exec.argc = toks[i].size;

	i++;
	for (j = 0; j < c->exec.argc; j++, i++) {
		c->exec.argv[j] = (json_token_str(json, &toks[i]));
		fprintf(stdout, "container init arg %d %s\n", j, c->exec.argv[j]);
	}

	return i;
}

static void container_free_cmd(struct hyper_container *c)
{
	int i;

	for (i = 0; i < c->exec.argc; i++) {
		free(c->exec.argv[i]);
	}

	free(c->exec.argv);
	c->exec.argv = NULL;
	c->exec.argc = 0;
}

static void container_free_volumes(struct hyper_container *c)
{
	int i;

	for (i = 0; i < c->vols_num; i++) {
		free(c->vols[i].device);
		free(c->vols[i].mountpoint);
		free(c->vols[i].fstype);
		free(c->vols[i].scsiaddr);
	}
	free(c->vols);
	c->vols = NULL;
	c->vols_num = 0;
}

static int container_parse_volumes(struct hyper_container *c, char *json, jsmntok_t *toks)
{
	int i = 0, j;

	if (toks[i].type != JSMN_ARRAY) {
		fprintf(stdout, "volume need array\n");
		return -1;
	}

	c->vols = calloc(toks[i].size, sizeof(*c->vols));
	if (c->vols == NULL) {
		fprintf(stderr, "allocate memory for volume failed\n");
		return -1;
	}

	c->vols_num = toks[i].size;
	fprintf(stdout, "volumes num %d\n", c->vols_num);

	i++;
	for (j = 0; j < c->vols_num; j++) {
		int i_volume, next_volume;

		if (toks[i].type != JSMN_OBJECT) {
			fprintf(stdout, "volume array need object\n");
			return -1;
		}
		next_volume = toks[i].size;
		i++;
		for (i_volume = 0; i_volume < next_volume; i_volume++, i++) {
			if (json_token_streq(json, &toks[i], "device")) {
				c->vols[j].device =
				(json_token_str(json, &toks[++i]));
				fprintf(stdout, "volume %d device %s\n", j, c->vols[j].device);
			} else if (json_token_streq(json, &toks[i], "addr")) {
				c->vols[j].scsiaddr = (json_token_str(json, &toks[++i]));
				fprintf(stdout, "volume %d scsi id %s\n", j, c->vols[j].scsiaddr);
			} else if (json_token_streq(json, &toks[i], "mount")) {
				c->vols[j].mountpoint =
				(json_token_str(json, &toks[++i]));
				fprintf(stdout, "volume %d mp %s\n", j, c->vols[j].mountpoint);
			} else if (json_token_streq(json, &toks[i], "fstype")) {
				c->vols[j].fstype =
				(json_token_str(json, &toks[++i]));
				fprintf(stdout, "volume %d fstype %s\n", j, c->vols[j].fstype);
			} else if (json_token_streq(json, &toks[i], "readOnly")) {
				if (!json_token_streq(json, &toks[++i], "false"))
					c->vols[j].readonly = 1;
				fprintf(stdout, "volume %d readonly %d\n", j, c->vols[j].readonly);
			} else {
				fprintf(stdout, "get unknown section %s in voulmes\n",
					json_token_str(json, &toks[i]));
				return -1;
			}
		}
	}

	return i;
}

void container_free_fsmap(struct hyper_container *c)
{
	int i;

	for (i = 0; i < c->maps_num; i++) {
		free(c->maps[i].source);
		free(c->maps[i].path);
	}
	free(c->maps);
	c->maps = NULL;
	c->maps_num = 0;
}

static int container_parse_fsmap(struct hyper_container *c, char *json, jsmntok_t *toks)
{
	int i = 0, j;

	if (toks[i].type != JSMN_ARRAY) {
		fprintf(stdout, "envs need array\n");
		return -1;
	}

	c->maps = calloc(toks[i].size, sizeof(*c->maps));
	if (c->maps == NULL) {
		fprintf(stderr, "allocate memory for fsmap failed\n");
		return -1;
	}

	c->maps_num = toks[i].size;
	fprintf(stdout, "fsmap num %d\n", c->maps_num);

	i++;
	for (j = 0; j < c->maps_num; j++) {
		int i_map, next_map;

		if (toks[i].type != JSMN_OBJECT) {
			fprintf(stdout, "fsmap array need object\n");
			return -1;
		}
		next_map = toks[i].size;
		i++;
		for (i_map = 0; i_map < next_map; i_map++, i++) {
			if (json_token_streq(json, &toks[i], "source")) {
				c->maps[j].source =
				(json_token_str(json, &toks[++i]));
				fprintf(stdout, "maps %d source %s\n", j, c->maps[j].source);
			} else if (json_token_streq(json, &toks[i], "path")) {
				c->maps[j].path =
				(json_token_str(json, &toks[++i]));
				fprintf(stdout, "maps %d path %s\n", j, c->maps[j].path);
			} else if (json_token_streq(json, &toks[i], "readOnly")) {
				if (!json_token_streq(json, &toks[++i], "false"))
					c->maps[j].readonly = 1;
				fprintf(stdout, "maps %d readonly %d\n", j, c->maps[j].readonly);
			} else {
				fprintf(stdout, "in maps incorrect %s\n",
					json_token_str(json, &toks[i]));
				return -1;
			}
		}
	}

	return i;
}

static void container_free_envs(struct hyper_container *c)
{
	int i;

	for (i = 0; i < c->envs_num; i++) {
		free(c->envs[i].env);
		free(c->envs[i].value);
	}

	free(c->envs);
	c->envs = NULL;
	c->envs_num = 0;
}

static int container_parse_envs(struct hyper_container *c, char *json, jsmntok_t *toks)
{
	int i = 0, j;

	if (toks[i].type != JSMN_ARRAY) {
		fprintf(stdout, "encs need array\n");
		return -1;
	}

	c->envs = calloc(toks[i].size, sizeof(*c->envs));
	if (c->envs == NULL) {
		fprintf(stderr, "allocate memory for env failed\n");
		return -1;
	}

	c->envs_num = toks[i].size;
	fprintf(stdout, "envs num %d\n", c->envs_num);

	i++;
	for (j = 0; j < c->envs_num; j++) {
		int i_env, next_env;

		if (toks[i].type != JSMN_OBJECT) {
			fprintf(stdout, "env array need object\n");
			return -1;
		}
		next_env = toks[i].size;
		i++;
		for (i_env = 0; i_env < next_env; i_env++, i++) {
			if (json_token_streq(json, &toks[i], "env")) {
				c->envs[j].env =
				(json_token_str(json, &toks[++i]));
				fprintf(stdout, "envs %d env %s\n", j, c->envs[j].env);
			} else if (json_token_streq(json, &toks[i], "value")) {
				c->envs[j].value =
				(json_token_str(json, &toks[++i]));
				fprintf(stdout, "envs %d value %s\n", j, c->envs[j].value);
			} else {
				fprintf(stdout, "get unknown section %s in envs\n",
					json_token_str(json, &toks[i]));
				return -1;
			}
		}
	}

	return i;
}

static void container_free_sysctl(struct hyper_container *c)
{
	int i;

	for (i = 0; i < c->sys_num; i++) {
		free(c->sys[i].path);
		free(c->sys[i].value);
	}

	free(c->sys);
	c->sys = NULL;
	c->sys_num = 0;
}

static int container_parse_sysctl(struct hyper_container *c, char *json, jsmntok_t *toks)
{
	int i = 0, j;
	char *p;

	if (toks[i].type != JSMN_OBJECT) {
		fprintf(stdout, "sysctl need object\n");
		return -1;
	}

	c->sys = calloc(toks[i].size, sizeof(*c->sys));
	if (c->sys == NULL) {
		fprintf(stderr, "allocate memory for sysctl failed\n");
		return -1;
	}

	c->sys_num = toks[i].size;
	fprintf(stdout, "sysctl size %d\n", c->sys_num);

	i++;
	for (j = 0; j < c->sys_num; j++) {
		c->sys[j].path = (json_token_str(json, &toks[++i]));
		while((p = strchr(c->sys[j].path, '.')) != NULL) {
			*p = '/';
		}
		c->sys[j].value = (json_token_str(json, &toks[++i]));
		fprintf(stdout, "sysctl %s:%s\n", c->sys[j].path, c->sys[j].value);
	}
	return i;
}

void hyper_free_container(struct hyper_container *c)
{
	free(c->id);
	c->id = NULL;

	free(c->rootfs);
	c->rootfs = NULL;

	free(c->image);
	c->image = NULL;

	free(c->scsiaddr);
	c->scsiaddr = NULL;

	free(c->workdir);
	c->workdir = NULL;

	free(c->fstype);
	c->fstype = NULL;

	free(c->exec.id);
	c->exec.id = NULL;

	container_free_volumes(c);
	container_free_envs(c);
	container_free_sysctl(c);
	container_free_fsmap(c);
	container_free_cmd(c);

	list_del_init(&c->list);
	free(c);
}

static int hyper_parse_container(struct hyper_pod *pod, struct hyper_container **container,
				 char *json, jsmntok_t *toks)
{
	int i = 0, j, next, next_container;
	struct hyper_container *c = NULL;
	jsmntok_t *t;

	if (toks[i].type != JSMN_OBJECT) {
		fprintf(stderr, "format incorrect\n");
		return -1;
	}

	c = calloc(1, sizeof(*c));
	if (c == NULL) {
		fprintf(stdout, "alloc memory for container failed\n");
		return -1;
	}

	c->exec.init = 1;
	c->exec.code = -1;
	c->exec.e.fd = -1;
	c->exec.errev.fd = -1;
	c->exec.ptyfd = -1;
	c->exec.errfd = -1;
	c->ns = -1;
	INIT_LIST_HEAD(&c->list);

	next_container = toks[i].size;
	fprintf(stdout, "next container %d\n", next_container);
	i++;
	for (j = 0; j < next_container; j++) {
		t = &toks[i];
		fprintf(stdout, "%d name %s\n", i, json_token_str(json, t));
		if (json_token_streq(json, t, "id") && t->size == 1) {
			c->id = (json_token_str(json, &toks[++i]));
			c->exec.id = strdup(c->id);
			fprintf(stdout, "container id %s\n", c->id);
			i++;
		} else if (json_token_streq(json, t, "cmd") && t->size == 1) {
			next = container_parse_cmd(c, json, &toks[++i]);
			if (next < 0)
				goto fail;
			i += next;
		} else if (json_token_streq(json, t, "rootfs") && t->size == 1) {
			c->rootfs = (json_token_str(json, &toks[++i]));
			fprintf(stdout, "container rootfs %s\n", c->rootfs);
			i++;
		} else if (json_token_streq(json, t, "tty") && t->size == 1) {
			c->exec.seq = json_token_ll(json, &toks[++i]);
			fprintf(stdout, "container seq %" PRIu64 "\n", c->exec.seq);
			i++;
		} else if (json_token_streq(json, t, "stderr") && t->size == 1) {
			c->exec.errseq = json_token_ll(json, &toks[++i]);
			fprintf(stdout, "container stderr seq %" PRIu64 "\n", c->exec.errseq);
			i++;
		} else if (json_token_streq(json, t, "workdir") && t->size == 1) {
			c->workdir = (json_token_str(json, &toks[++i]));
			fprintf(stdout, "container workdir %s\n", c->workdir);
			i++;
		} else if (json_token_streq(json, t, "image") && t->size == 1) {
			c->image = (json_token_str(json, &toks[++i]));
			fprintf(stdout, "container image %s\n", c->image);
			i++;
		} else if (json_token_streq(json, t, "addr") && t->size == 1) {
			c->scsiaddr = (json_token_str(json, &toks[++i]));
			fprintf(stdout, "container image scsi id %s\n", c->scsiaddr);
			i++;
		} else if (json_token_streq(json, t, "fstype") && t->size == 1) {
			c->fstype = (json_token_str(json, &toks[++i]));
			fprintf(stdout, "container fstype %s\n", c->fstype);
			i++;
		} else if (json_token_streq(json, t, "volumes") && t->size == 1) {
			next = container_parse_volumes(c, json, &toks[++i]);
			if (next < 0)
				goto fail;
			i += next;
		} else if (json_token_streq(json, t, "fsmap") && t->size == 1) {
			next = container_parse_fsmap(c, json, &toks[++i]);
			if (next < 0)
				goto fail;
			i += next;
		} else if (json_token_streq(json, t, "envs") && t->size == 1) {
			next = container_parse_envs(c, json, &toks[++i]);
			if (next < 0)
				goto fail;
			i += next;
		} else if (json_token_streq(json, t, "sysctl") && t->size == 1) {
			next = container_parse_sysctl(c, json, &toks[++i]);
			if (next < 0)
				goto fail;
			i += next;
		} else if (json_token_streq(json, t, "restartPolicy") && t->size == 1) {
			fprintf(stdout, "restart policy %s\n", json_token_str(json, &toks[++i]));
			i++;
		} else {
			fprintf(stdout, "get unknown section %s in container\n",
				json_token_str(json, t));
			goto fail;
		}
	}

	*container = c;
	return i;
fail:
	hyper_free_container(c);
	*container = NULL;
	return -1;
}

static int hyper_parse_containers(struct hyper_pod *pod, char *json, jsmntok_t *toks)
{
	int i = 0, j = 0, next, c_num;
	struct hyper_container *c, *n;

	if (toks[i].type != JSMN_ARRAY) {
		fprintf(stdout, "format incorrect\n");
		return -1;
	}

	c_num = toks[i].size;
	fprintf(stdout, "container count %d\n", c_num);

	i++;
	for (j = 0; j < c_num; j++) {
		next = hyper_parse_container(pod, &c, json, toks + i);
		if (next < 0)
			goto fail;

		/* Pod created containers, Add to list immediately */
		list_add_tail(&c->list, &pod->containers);
		i += next;
	}

	return i;
fail:
	list_for_each_entry_safe(c, n, &pod->containers, list)
		hyper_free_container(c);

	return -1;
}

static int hyper_parse_interfaces(struct hyper_pod *pod, char *json, jsmntok_t *toks)
{
	int i = 0, j, next_if;
	struct hyper_interface *iface;

	if (toks[i].type != JSMN_ARRAY) {
		fprintf(stdout, "interfaces need array\n");
		return -1;
	}

	pod->i_num = toks[i].size;
	fprintf(stdout, "network interfaces num %d\n", pod->i_num);

	pod->iface = calloc(pod->i_num, sizeof(*iface));
	if (pod->iface == NULL) {
		fprintf(stdout, "alloc memory for interface failed\n");
		return -1;
	}

	i++;
	for (j = 0; j < pod->i_num; j++) {
		int i_if;
		iface = &pod->iface[j];

		if (toks[i].type != JSMN_OBJECT) {
			fprintf(stdout, "network array need object\n");
			return -1;
		}
		next_if = toks[i].size;

		i++;
		for (i_if = 0; i_if < next_if; i_if++, i++) {
			if (json_token_streq(json, &toks[i], "device")) {
				iface->device = (json_token_str(json, &toks[++i]));
				fprintf(stdout, "net device is %s\n", iface->device);
			} else if (json_token_streq(json, &toks[i], "ipAddress")) {
				iface->ipaddr = (json_token_str(json, &toks[++i]));
				fprintf(stdout, "net ipaddress is %s\n", iface->ipaddr);
			} else if (json_token_streq(json, &toks[i], "netMask")) {
				iface->mask = (json_token_str(json, &toks[++i]));
				fprintf(stdout, "net mask is %s\n", iface->mask);
			} else {
				fprintf(stderr, "get unknown section %s in interfaces\n",
					json_token_str(json, &toks[i]));
				return -1;
			}
		}
	}

	return i;
}

static int hyper_parse_routes(struct hyper_pod *pod, char *json, jsmntok_t *toks)
{
	int i = 0, j, next_rt;
	struct hyper_route *rt;

	if (toks[i].type != JSMN_ARRAY) {
		fprintf(stdout, "routes need array\n");
		return -1;
	}

	pod->r_num = toks[i].size;
	fprintf(stdout, "network routes num %d\n", pod->r_num);

	pod->rt = calloc(pod->r_num, sizeof(*rt));
	if (pod->rt == NULL) {
		fprintf(stdout, "alloc memory for router failed\n");
		return -1;
	}

	i++;
	for (j = 0; j < pod->r_num; j++) {
		int i_rt;

		rt = &pod->rt[j];
		if (toks[i].type != JSMN_OBJECT) {
			fprintf(stdout, "routes array need object\n");
			return -1;
		}
		next_rt = toks[i].size;

		i++;
		for (i_rt = 0; i_rt < next_rt; i_rt++, i++) {
			if (json_token_streq(json, &toks[i], "dest")) {
				rt->dst = (json_token_str(json, &toks[++i]));
				fprintf(stdout, "route %d dest is %s\n", j, rt->dst);
			} else if (json_token_streq(json, &toks[i], "gateway")) {
				rt->gw = (json_token_str(json, &toks[++i]));
				fprintf(stdout, "route %d gateway is %s\n", j, rt->gw);
			} else if (json_token_streq(json, &toks[i], "device")) {
				rt->device = (json_token_str(json, &toks[++i]));
				fprintf(stdout, "route %d device is %s\n", j, rt->device);
			} else {
				fprintf(stderr, "get unknown section %s in routes\n",
					json_token_str(json, &toks[i]));
				return -1;
			}
		}
	}

	return i;
}

static int hyper_parse_dns(struct hyper_pod *pod, char *json, jsmntok_t *toks)
{
	int i = 0, j;

	if (toks[i].type != JSMN_ARRAY) {
		fprintf(stdout, "Dns format incorrect\n");
		return -1;
	}

	pod->d_num = toks[i].size;
	fprintf(stdout, "dns count %d\n", pod->d_num);

	pod->dns = calloc(pod->d_num, sizeof(*pod->dns));
	if (pod->dns == NULL) {
		fprintf(stdout, "alloc memory for dns failed\n");
		return -1;
	}

	i++;
	for (j = 0; j < pod->d_num; j++, i++) {
		pod->dns[j] = (json_token_str(json, &toks[i]));
		fprintf(stdout, "pod dns %d: %s\n", j, pod->dns[j]);
	}

	return i;
}

int hyper_parse_pod(struct hyper_pod *pod, char *json, int length)
{
	int i, n, next = -1;
	jsmn_parser p;
	int toks_num = 100;
	jsmntok_t *toks = NULL;

realloc:
	toks = realloc(toks, toks_num * sizeof(jsmntok_t));
	if (toks == NULL) {
		fprintf(stderr, "allocate tokens for pod failed\n");
		goto out;
	}

	fprintf(stdout, "call hyper_start_pod, json %s, len %d\n", json, length);
	jsmn_init(&p);
	n = jsmn_parse(&p, json, length, toks, toks_num);
	if (n < 0) {
		fprintf(stdout, "jsmn parse failed, n is %d\n", n);
		if (n == JSMN_ERROR_NOMEM) {
			toks_num *= 2;
			goto realloc;
		}

		goto out;
	}

	pod->policy = POLICY_NEVER;

	fprintf(stdout, "jsmn parse successed, n is %d\n", n);
	next = 0;
	for (i = 0; i < n;) {
		jsmntok_t *t = &toks[i];

		fprintf(stdout, "token %d, type is %d, size is %d\n", i, t->type, t->size);

		if (t->type != JSMN_STRING) {
			i++;
			continue;
		}

		if (json_token_streq(json, t, "containers") && t->size == 1) {
			next = hyper_parse_containers(pod, json, &toks[++i]);
			if (next < 0)
				goto out;

			i += next;
		} else if (json_token_streq(json, t, "interfaces") && t->size == 1) {
			next = hyper_parse_interfaces(pod, json, &toks[++i]);
			if (next < 0)
				goto out;

			i += next;
		} else if (json_token_streq(json, t, "routes") && t->size == 1) {
			next = hyper_parse_routes(pod, json, &toks[++i]);
			if (next < 0)
				goto out;

			i += next;
		} else if (json_token_streq(json, t, "dns") && t->size == 1) {
			next = hyper_parse_dns(pod, json, &toks[++i]);
			if (next < 0)
				goto out;

			i += next;
		} else if (json_token_streq(json, t, "shareDir") && t->size == 1) {
			pod->share_tag = (json_token_str(json, &toks[++i]));
			fprintf(stdout, "share tag is %s\n", pod->share_tag);
			i++;
		} else if (json_token_streq(json, t, "hostname") && t->size == 1) {
			pod->hostname = (json_token_str(json, &toks[++i]));
			fprintf(stdout, "hostname is %s\n", pod->hostname);
			i++;
		} else if (json_token_streq(json, t, "restartPolicy") && t->size == 1) {
			i++;
			if (json_token_streq(json, &toks[i], "always") && toks[i].size == 1)
				pod->policy = POLICY_ALWAYS;
			else if (json_token_streq(json, &toks[i], "onFailure") && toks[i].size == 1)
				pod->policy = POLICY_ONFAILURE;
			fprintf(stdout, "restartPolicy is %" PRIu8 "\n", pod->policy);
			i++;
		} else {
			fprintf(stdout, "get unknown section %s in pod\n",
				json_token_str(json, &toks[i]));
			next = -1;
			break;
		}
	}

out:
	free(toks);
	return next;
}

struct hyper_container *hyper_parse_new_container(struct hyper_pod *pod, char *json, int length)
{
	int n;
	jsmn_parser p;
	int toks_num = 100;
	jsmntok_t *toks = NULL;
	struct hyper_container *c = NULL;

realloc:
	toks = realloc(toks, toks_num * sizeof(jsmntok_t));
	if (toks == NULL) {
		fprintf(stderr, "allocate tokens for new container failed\n");
		goto fail;
	}

	jsmn_init(&p);
	n = jsmn_parse(&p, json, length, toks, toks_num);
	if (n < 0) {
		fprintf(stdout, "jsmn parse failed, n is %d\n", n);
		if (n == JSMN_ERROR_NOMEM) {
			toks_num *= 2;
			goto realloc;
		}

		goto fail;
	}

	if (hyper_parse_container(pod, &c, json, toks) < 0)
		goto fail;

	free(toks);
	return c;

fail:
	free(toks);
	return NULL;
}

int hyper_parse_kill_container(struct hyper_killer *killer, char *json, int length)
{
	int i, n, ret = -1;
	jsmn_parser p;
	int toks_num = 10;
	jsmntok_t *toks = NULL;

	memset(killer, 0, sizeof(*killer));
realloc:
	toks = realloc(toks, toks_num * sizeof(jsmntok_t));
	if (toks == NULL) {
		fprintf(stderr, "allocate tokens for kill container failed\n");
		goto out;
	}

	jsmn_init(&p);

	n = jsmn_parse(&p, json, length,  toks, toks_num);
	if (n < 0) {
		fprintf(stdout, "jsmn parse failed, n is %d\n", n);
		if (n == JSMN_ERROR_NOMEM) {
			toks_num *= 2;
			goto realloc;
		}

		goto out;
	}

	for (i = 0; i < n; i++) {
		jsmntok_t *t = &toks[i];

		if (t->type != JSMN_STRING)
			continue;

		if (i++ == n)
			goto fail;

		if (json_token_streq(json, t, "container")) {
			if (toks[i].type != JSMN_STRING)
				goto fail;
			killer->id = (json_token_str(json, &toks[i]));
		} else if (json_token_streq(json, t, "signal")) {
			if (toks[i].type != JSMN_PRIMITIVE)
				goto fail;
			killer->signal = json_token_int(json, &toks[i]);
		} else {
			fprintf(stderr, "get unknown section %s in kill container\n",
				json_token_str(json, t));
			goto fail;
		}
	}

	ret = 0;
out:
	free(toks);
	return ret;
fail:
	free(killer->id);
	killer->id = NULL;
	goto out;
}

int hyper_parse_winsize(struct hyper_win_size *ws, char *json, int length)
{
	int i, n, ret = -1;
	jsmn_parser p;
	int toks_num = 10;
	jsmntok_t *toks = NULL;

	memset(ws, 0, sizeof(*ws));
realloc:
	toks = realloc(toks, toks_num * sizeof(jsmntok_t));
	if (toks == NULL) {
		fprintf(stderr, "allocate tokens for winsize failed\n");
		goto out;
	}

	jsmn_init(&p);

	n = jsmn_parse(&p, json, length,  toks, toks_num);
	if (n < 0) {
		fprintf(stdout, "jsmn parse failed, n is %d\n", n);
		if (n == JSMN_ERROR_NOMEM) {
			toks_num *= 2;
			goto realloc;
		}

		goto out;
	}

	for (i = 0; i < n; i++) {
		jsmntok_t *t = &toks[i];

		if (t->type != JSMN_STRING)
			continue;

		if (i++ == n)
			goto fail;

		if (json_token_streq(json, t, "tty")) {
			if (toks[i].type != JSMN_STRING)
				goto fail;
			ws->tty = (json_token_str(json, &toks[i]));
		} else if (json_token_streq(json, t, "seq")) {
			if (toks[i].type != JSMN_PRIMITIVE)
				goto fail;
			ws->seq = json_token_ll(json, &toks[i]);
		} else if (json_token_streq(json, t, "row")) {
			if (toks[i].type != JSMN_PRIMITIVE)
				goto fail;
			ws->row = json_token_int(json, &toks[i]);
		} else if (json_token_streq(json, t, "column")) {
			if (toks[i].type != JSMN_PRIMITIVE)
				goto fail;
			ws->column = json_token_int(json, &toks[i]);
		} else {
			fprintf(stderr, "get unknown section %s in winsize\n",
				json_token_str(json, t));
			goto fail;
		}
	}

	ret = 0;
out:
	free(toks);
	return ret;
fail:
	free(ws->tty);
	ws->tty = NULL;
	goto out;
}

struct hyper_exec *hyper_parse_execcmd(char *json, int length)
{
	int i, j, n, has_seq = 0;
	struct hyper_exec *exec = NULL;

	jsmn_parser p;
	int toks_num = 10;
	jsmntok_t *toks = NULL;

realloc:
	toks = realloc(toks, toks_num * sizeof(jsmntok_t));
	if (toks == NULL) {
		fprintf(stderr, "allocate tokens for execcmd failed\n");
		goto fail;
	}

	jsmn_init(&p);
	n = jsmn_parse(&p, json, length,  toks, toks_num);
	if (n < 0) {
		fprintf(stdout, "jsmn parse failed, n is %d\n", n);
		if (n == JSMN_ERROR_NOMEM) {
			toks_num *= 2;
			goto realloc;
		}
		goto out;
	}

	exec = calloc(1, sizeof(*exec));
	if (exec == NULL) {
		fprintf(stderr, "allocate memory for exec cmd failed\n");
		goto out;
	}

	exec->ptyfd = -1;
	exec->errfd = -1;
	exec->e.fd = -1;
	exec->errev.fd = -1;
	INIT_LIST_HEAD(&exec->list);

	for (i = 0, j = 0; i < n; i++) {
		jsmntok_t *t = &toks[i];

		if (t->type != JSMN_STRING)
			continue;

		if (json_token_streq(json, t, "container")) {
			exec->id = (json_token_str(json, &toks[++i]));
			fprintf(stdout, "get container %s\n", exec->id);
		} else if (json_token_streq(json, t, "seq")) {
			has_seq = 1;
			exec->seq = json_token_ll(json, &toks[++i]);
			fprintf(stdout, "get seq %"PRIu64"\n", exec->seq);
		} else if (json_token_streq(json, t, "cmd")) {
			if (toks[++i].type != JSMN_ARRAY) {
				fprintf(stdout, "execcmd need array\n");
				goto fail;
			}

			exec->argv = calloc(toks[i].size + 1, sizeof(*exec->argv));
			if (exec->argv == NULL) {
				fprintf(stdout, "allocate memory for exec cmd argv failed\n");
				goto fail;
			}
			exec->argc = toks[i].size;
			exec->argv[exec->argc] = NULL;
		} else if (j < exec->argc) {
			exec->argv[j++] = (json_token_str(json, &toks[i]));
			fprintf(stdout, "argv %d, %s\n", j - 1, exec->argv[j - 1]);
		} else {
			fprintf(stderr, "get unknown section %s in exec cmd\n",
				json_token_str(json, t));
			goto fail;
		}
	}

	if (!has_seq) {
		fprintf(stderr, "execcmd format error, has no seq\n");
		goto fail;
	}

out:
	free(toks);
	return exec;
fail:
	free(exec->id);
	for (i = 0; i < exec->argc; i++)
		free(exec->argv[i]);

	free(exec->argv);
	free(exec);

	exec = NULL;
	goto out;
}

int hyper_parse_write_file(struct hyper_writter *writter, char *json, int length)
{
	int i, n, ret = -1;

	jsmn_parser p;
	int toks_num = 10;
	jsmntok_t *toks = NULL;

	memset(writter, 0, sizeof(*writter));

	toks = calloc(toks_num, sizeof(jsmntok_t));
	if (toks == NULL) {
		fprintf(stderr, "fail to allocate tokens for write file cmd\n");
		goto fail;
	}

	jsmn_init(&p);
	n = jsmn_parse(&p, json, length,  toks, toks_num);
	/* Must be json first */
	if (n <= 0) {
		fprintf(stdout, "jsmn parse failed, n is %d\n", n);
		goto fail;
	}

	writter->len = length - toks[0].end;
	writter->data = malloc(writter->len);

	if (writter->data == NULL) {
		fprintf(stderr, "fail to allocate memory for writter data\n");
		goto fail;
	}

	memcpy(writter->data, json + toks[0].end, writter->len);
	fprintf(stdout, "writefile get data len %d %s\n", writter->len, writter->data);

	for (i = 0; i < n; i++) {
		jsmntok_t *t = &toks[i];

		if (t->type != JSMN_STRING)
			continue;

		if (i++ == n)
			goto fail;

		if (json_token_streq(json, t, "container")) {
			writter->id = (json_token_str(json, &toks[i]));
			fprintf(stdout, "writefile get container %s\n", writter->id);
		} else if (json_token_streq(json, t, "file")) {
			writter->file = (json_token_str(json, &toks[i]));
			fprintf(stdout, "writefile get file %s\n", writter->file);
		} else {
			fprintf(stderr, "get unknown section %s in writefile\n",
				json_token_str(json, t));
			goto fail;
		}
	}

	if (writter->id == NULL || writter->file == NULL) {
		fprintf(stderr, "writefile format incorrect\n");
		goto fail;
	}

	ret = 0;
out:
	free(toks);
	return ret;
fail:
	free(writter->id);
	free(writter->file);
	free(writter->data);
	goto out;
}

int hyper_parse_read_file(struct hyper_reader *reader, char *json, int length)
{
	int i, n, ret = -1;

	jsmn_parser p;
	int toks_num = 10;
	jsmntok_t *toks = NULL;

	memset(reader, 0, sizeof(*reader));

	toks = calloc(toks_num, sizeof(jsmntok_t));
	if (toks == NULL) {
		fprintf(stderr, "fail to allocate tokens for read file cmd\n");
		ret = -1;
		goto fail;
	}

	jsmn_init(&p);
	n = jsmn_parse(&p, json, length,  toks, toks_num);
	if (n < 0) {
		fprintf(stdout, "jsmn parse failed, n is %d\n", n);
		ret = -1;
		goto fail;
	}

	for (i = 0; i < n; i++) {
		jsmntok_t *t = &toks[i];

		if (t->type != JSMN_STRING)
			continue;

		if (i++ == n)
			goto fail;

		if (json_token_streq(json, t, "container")) {
			reader->id = (json_token_str(json, &toks[i]));
			fprintf(stdout, "readfile get container %s\n", reader->id);
		} else if (json_token_streq(json, t, "file")) {
			reader->file = (json_token_str(json, &toks[i]));
			fprintf(stdout, "readfile get file %s\n", reader->file);
		} else {
			fprintf(stdout, "get unknown section %s in readfile\n",
				json_token_str(json, t));
			goto fail;
		}
	}

	if (reader->id == NULL || reader->file == NULL) {
		fprintf(stderr, "readfile format incorrect\n");
		goto fail;
	}

	ret = 0;
out:
	free(toks);
	return ret;
fail:
	free(reader->id);
	reader->id = NULL;
	free(reader->file);
	reader->file = NULL;
	goto out;
}