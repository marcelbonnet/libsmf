#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <arpa/inet.h>
#include "smf.h"

/* Reference: http://www.borg.com/~jglatt/tech/midifile.htm */

struct chunk_header_struct {
	char		id[4];
	uint32_t	length; 
} __attribute__((__packed__));

struct mthd_chunk_struct {
	struct chunk_header_struct	mthd_header;
	uint16_t			format;
	uint16_t			number_of_tracks;
	uint16_t			division;
} __attribute__((__packed__));

static smf_t *
smf_new(void)
{
	smf_t *smf = malloc(sizeof(smf_t));

	assert(smf != NULL);

	memset(smf, 0, sizeof(smf_t));

	return smf;
}

static smf_track_t *
smf_track_new(smf_t *smf)
{
	smf_track_t *track = malloc(sizeof(smf_track_t));

	assert(track != NULL);

	memset(track, 0, sizeof(smf_track_t));

	track->smf = smf;

	return track;
}

static smf_event_t *
smf_event_new(smf_track_t *track)
{
	smf_event_t *event = malloc(sizeof(smf_event_t));

	assert(event != NULL);

	memset(event, 0, sizeof(smf_event_t));

	event->track = track;

	return event;
}

static struct chunk_header_struct *
next_chunk(smf_t *smf)
{
	struct chunk_header_struct *chunk;

	void *next_chunk_ptr = (unsigned char *)smf->buffer + smf->next_chunk_offset;

	chunk = (struct chunk_header_struct *)next_chunk_ptr;

	smf->next_chunk_offset += sizeof(struct chunk_header_struct) + ntohl(chunk->length);

	if (smf->next_chunk_offset > smf->buffer_length)
		return NULL;

	return chunk;
}

static int
signature_matches(const struct chunk_header_struct *chunk, const char *signature)
{
	if (chunk->id[0] == signature[0] && chunk->id[1] == signature[1] && chunk->id[2] == signature[2] && chunk->id[3] == signature[3])
		return 1;

	return 0;
}

static int
parse_mthd_header(smf_t *smf)
{
	int len;
	struct chunk_header_struct *mthd;

	/* Make sure compiler didn't do anything stupid. */
	assert(sizeof(struct chunk_header_struct) == 8);

	mthd = next_chunk(smf);

	if (mthd == NULL) {
		fprintf(stderr, "Truncated file.\n");

		return 1;
	}

	if (!signature_matches(mthd, "MThd")) {
		fprintf(stderr, "MThd signature not found, is that a MIDI file?\n");
		
		return 2;
	}

	len = ntohl(mthd->length);
	if (len != 6) {
		fprintf(stderr, "MThd chunk length %d, should be 6, please report this.\n", len);

		return 3;
	}

	return 0;
}

static int
parse_mthd_chunk(smf_t *smf)
{
	signed char first_byte_of_division, second_byte_of_division;

	struct mthd_chunk_struct *mthd;

	assert(sizeof(struct mthd_chunk_struct) == 14);

	if (parse_mthd_header(smf))
		return 1;

	mthd = (struct mthd_chunk_struct *)smf->buffer;

	smf->format = ntohs(mthd->format);
	smf->number_of_tracks = ntohs(mthd->number_of_tracks);

	/* XXX: endianess? */
	first_byte_of_division = *((signed char *)&(mthd->division));
	second_byte_of_division = *((signed char *)&(mthd->division) + 1);

	if (first_byte_of_division >= 0) {
		smf->ppqn = ntohs(mthd->division);
		smf->frames_per_second = 0;
		smf->resolution = 0;
	} else {
		smf->ppqn = 0;
		smf->frames_per_second = - first_byte_of_division;
		smf->resolution = second_byte_of_division;
	}
	
	return 0;
}

static void
print_mthd(smf_t *smf)
{
	fprintf(stderr, "**** Values from MThd ****\n");

	switch (smf->format) {
		case 0:
			fprintf(stderr, "Format: 0 (single track)\n");
			break;

		case 1:
			fprintf(stderr, "Format: 1 (sevaral simultaneous tracks)\n");
			break;

		case 2:
			fprintf(stderr, "Format: 2 (sevaral independent tracks)\n");
			break;

		default:
			fprintf(stderr, "Format: %d (INVALID FORMAT)\n", smf->format);
			break;
	}

	fprintf(stderr, "Number of tracks: %d\n", smf->number_of_tracks);
	if (smf->format == 0 && smf->number_of_tracks != 0)
		fprintf(stderr, "Warning: number of tracks is %d, but this is a single track file.\n", smf->number_of_tracks);

	if (smf->ppqn != 0)
		fprintf(stderr, "Division: %d PPQN\n", smf->ppqn);
	else
		fprintf(stderr, "Division: %d FPS, %d resolution\n", smf->frames_per_second, smf->resolution);
}

static smf_event_t *
parse_next_event(smf_track_t *track)
{
	int time = 0, status, i;
	unsigned char *c, *start, *actual_event_start;

	smf_event_t *event = smf_event_new(track);
	
	start = (unsigned char *)track->buffer + track->next_event_offset;
	c = start;

	//fprintf(stderr, "*c = 0x%x; next *c = 0x%x;\n", *c, *(c + 1));
	
	/* First, extract the time. */
	for (;;) {
		time = (time << 7) + (*c & 0x7F);

		if (*c & 0x80)
			c++;
		else
			break;
	};

	event->time = time;

	//fprintf(stderr, "time = %d; c - start = %d;\n", time, c - start);

	/* Now, extract the actual event. */
	c++;
	actual_event_start = c;

	/* Is the first byte the status byte? */
	if (*c & 0x80) {
		status = *c;
		track->last_status = *c;
		c++;

	} else {
		/* No, we use running status then. */
		status = track->last_status;
	}

	fprintf(stderr, "time %d; status 0x%x; ", time, status);

	if ((status & 0x80) == 0) {
		fprintf(stderr, "Bad status (MSB is zero).\n");
		return NULL;
	}

	event->midi_buffer[0] = status;

	/* Is this a "meta event"? */
	if (status == 0xFF) {
		/* 0xFF 0xwhatever 0xlength + the actual length. */
		int len = *(c + 1) + 3;

		//fprintf(stderr, "len %d\n", len);

		for (i = 1; i < len; i++, c++) {
			if (i >= 1024) {
				fprintf(stderr, "Whoops, meta event too long.\n");
				continue;
			}

			fprintf(stderr, "0x%x ", *c);
			event->midi_buffer[i] = *c;
		}

	} else {

		/* XXX: running status does not really work that way. */
		/* Copy the rest of the MIDI event into buffer. */
		for (i = 1; (*(c + 1) & 0x80) == 0; i++, c++) {
			if (i >= 1024) {
				fprintf(stderr, "Whoops, MIDI event too long.\n");
				continue;
			}

			fprintf(stderr, "0x%x ", *c);
			event->midi_buffer[i] = *c;
		}
	}
	
	fprintf(stderr, "\ntime length %d; actual event length %d;\n", actual_event_start - start, c - actual_event_start);

	track->next_event_offset += c - start;

	return event;
}

static void
print_event(smf_event_t *event)
{
	fprintf(stderr, "Event: time %d; status 0x%x;\n", event->time, event->midi_buffer[0]);

	if (event->midi_buffer[0] == 0xFF) {
		switch (event->midi_buffer[1]) {
			case 0x00:
				fprintf(stderr, "Sequence Number\n");
				break;

			case 0x01:
				fprintf(stderr, "Text\n");
				break;

			case 0x02:
				fprintf(stderr, "Copyright\n");
				break;

			case 0x03:
				fprintf(stderr, "Sequence/Track Name\n");
				break;

			case 0x04:
				fprintf(stderr, "Instrument\n");
				break;

			case 0x05:
				fprintf(stderr, "Lyric\n");
				break;

			case 0x06:
				fprintf(stderr, "Marker\n");
				break;

			case 0x07:
				fprintf(stderr, "Cue Point\n");
				break;

			case 0x08:
				fprintf(stderr, "Program Name\n");
				break;

			case 0x09:
				fprintf(stderr, "Device (Port) Name\n");
				break;

			case 0x2F:
				fprintf(stderr, "End Of Track\n");
				break;

			case 0x51:
				fprintf(stderr, "Tempo\n");
				break;

			case 0x54:
				fprintf(stderr, "SMPTE Offset\n");
				break;

			case 0x58:
				fprintf(stderr, "Time Signature\n");
				break;

			case 0x59:
				fprintf(stderr, "Key Signature\n");
				break;

			case 0x7F:
				fprintf(stderr, "Proprietary Event\n");
				break;

			default:
				fprintf(stderr, "Uknown event.\n");
				break;
		}
	}
}

static int
parse_mtrk_header(smf_track_t *track)
{
	struct chunk_header_struct *mtrk;

	/* Make sure compiler didn't do anything stupid. */
	assert(sizeof(struct chunk_header_struct) == 8);
	assert(track->smf != NULL);

	mtrk = next_chunk(track->smf);

	if (mtrk == NULL) {
		fprintf(stderr, "Truncated file.\n");

		return 1;
	}

	if (!signature_matches(mtrk, "MTrk")) {
		fprintf(stderr, "MTrk signature not found, skipping chunk.\n");
		
		return 2;
	}

	track->buffer = mtrk;
	track->buffer_length = sizeof(struct chunk_header_struct) + ntohl(mtrk->length);
	track->next_event_offset = sizeof(struct chunk_header_struct);

	return 0;
}

int
is_end_of_track(const smf_event_t *event)
{
	if (event->midi_buffer[0] == 0xFF && event->midi_buffer[1] == 0x2F)
		return 1;

	return 0;
}

static int
parse_mtrk_chunk(smf_track_t *track)
{
	if (parse_mtrk_header(track))
		return 1;

	fprintf(stderr, "*** Parsing track ***\n");
	for (;;) {
		smf_event_t *event = parse_next_event(track);

		if (is_end_of_track(event)) {
		//	print_event(event);
			free(event);
			break;
		}

		if (event == NULL)
			return 2;

//		print_event(event);
		free(event);
	}

	return 0;
}

static int
load_file_into_buffer(smf_t *smf, const char *file_name)
{
	smf->stream = fopen(file_name, "r");
	if (smf->stream == NULL) {
		perror("Cannot open input file");

		return 1;
	}

	if (fseek(smf->stream, 0, SEEK_END)) {
		perror("fseek(3) failed");

		return 2;
	}

	smf->buffer_length = ftell(smf->stream);
	if (smf->buffer_length == -1) {
		perror("ftell(3) failed");

		return 3;
	}

	if (fseek(smf->stream, 0, SEEK_SET)) {
		perror("fseek(3) failed");

		return 4;
	}

	smf->buffer = malloc(smf->buffer_length);
	if (smf->buffer == NULL) {
		perror("malloc(3) failed");

		return 5;
	}

	if (fread(smf->buffer, 1, smf->buffer_length, smf->stream) != smf->buffer_length) {
		perror("fread(3) failed");

		return 6;
	}

	return 0;
}

smf_t *
smf_open(const char *file_name)
{
	int i;

	smf_t *smf = smf_new();

	if (load_file_into_buffer(smf, file_name))
		return NULL;

	if (parse_mthd_chunk(smf))
		return NULL;

	print_mthd(smf);

	for (i = 0; i < smf->number_of_tracks; i++) {
		smf_track_t *track = smf_track_new(smf);

		if (parse_mtrk_chunk(track))
			return NULL;
	}

	return smf;
}

void
smf_close(smf_t *smf)
{
	if (smf->buffer != NULL) {
		free(smf->buffer);
		smf->buffer = NULL;
	}

	if (smf->stream != NULL) {
		fclose(smf->stream);
		smf->stream = NULL;
	}
}

