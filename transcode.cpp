#include <iostream>
#include <string>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>

extern "C"
{
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
#include <libavutil/opt.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
}

static int init_filter_graph(AVFilterGraph **graph, AVFilterContext **src,
                             AVFilterContext **sink, AVCodecContext *encctx, AVCodecContext *decctx)
{
    AVFilterGraph *filter_graph;
    AVFilterContext *abuffer_ctx;
    const AVFilter *abuffer;
    AVFilterContext *volume_ctx;
    const AVFilter *volume;
    AVFilterContext *abuffersink_ctx;
    const AVFilter *abuffersink;
    const AVFilter *aformat;
    AVFilterContext *aformat_ctx;
    char *strbuf;

    AVDictionary *options_dict = NULL;
    char ch_layout[128];

    int err;

    /* Create a new filtergraph, which will contain all the filters. */
    filter_graph = avfilter_graph_alloc();
    if (!filter_graph)
    {
        fprintf(stderr, "Unable to create filter graph.\n");
        return AVERROR(ENOMEM);
    }

    /* Create the abuffer filter;
     * it will be used for feeding the data into the graph. */
    abuffer = avfilter_get_by_name("abuffer");
    if (!abuffer)
    {
        fprintf(stderr, "Could not find the abuffer filter.\n");
        return AVERROR_FILTER_NOT_FOUND;
    }

    abuffer_ctx = avfilter_graph_alloc_filter(filter_graph, abuffer, "src");
    if (!abuffer_ctx)
    {
        fprintf(stderr, "Could not allocate the abuffer instance.\n");
        return AVERROR(ENOMEM);
    }

    /* Set the filter options through the AVOptions API. */
    av_channel_layout_describe(&decctx->ch_layout, ch_layout, sizeof(ch_layout));
    av_opt_set(abuffer_ctx, "channel_layout", ch_layout, AV_OPT_SEARCH_CHILDREN);
    av_opt_set(abuffer_ctx, "sample_fmt", av_get_sample_fmt_name(decctx->sample_fmt), AV_OPT_SEARCH_CHILDREN);
    av_opt_set_q(abuffer_ctx, "time_base", (AVRational){1, decctx->sample_rate}, AV_OPT_SEARCH_CHILDREN);
    av_opt_set_int(abuffer_ctx, "sample_rate", decctx->sample_rate, AV_OPT_SEARCH_CHILDREN);

    /* Now initialize the filter; we pass NULL options, since we have already
     * set all the options above. */
    err = avfilter_init_str(abuffer_ctx, NULL);
    if (err < 0)
    {
        fprintf(stderr, "Could not initialize the abuffer filter.\n");
        return err;
    }

    /* Create volume filter. */
    volume = avfilter_get_by_name("volume");
    if (!volume)
    {
        fprintf(stderr, "Could not find the volume filter.\n");
        return AVERROR_FILTER_NOT_FOUND;
    }

    volume_ctx = avfilter_graph_alloc_filter(filter_graph, volume, "volume");
    if (!volume_ctx)
    {
        fprintf(stderr, "Could not allocate the volume instance.\n");
        return AVERROR(ENOMEM);
    }

    /* A different way of passing the options is as key/value pairs in a
     * dictionary. */
    av_dict_set(&options_dict, "volume", AV_STRINGIFY(1.0), 0);
    // you must set it if you use fixed/signed sample format, e.g pcm_s16le
    av_dict_set(&options_dict, "precision", "fixed", 0);
    err = avfilter_init_dict(volume_ctx, &options_dict);
    av_dict_free(&options_dict);
    if (err < 0)
    {
        fprintf(stderr, "Could not initialize the volume filter.\n");
        return err;
    }

    // create aformat filter
    aformat = avfilter_get_by_name("aformat");
    if (!aformat)
    {
        snprintf(strbuf, 1024, "Could not find the aformat filter!");
        return 2;
    }

    aformat_ctx = avfilter_graph_alloc_filter(filter_graph, aformat, "aformat");
    if (err < 0)
    {
        av_log(NULL, AV_LOG_ERROR, "unable to create aformat filter\n");
        return err;
    }
    err = avfilter_init_str(aformat_ctx, NULL);
    if (err < 0)
    {
        fprintf(stderr, "Could not initialize the aformat instance.\n");
        return err;
    }

    /* Finally create the abuffersink filter;
     * it will be used to get the filtered data out of the graph. */
    abuffersink = avfilter_get_by_name("abuffersink");
    if (!abuffersink)
    {
        fprintf(stderr, "Could not find the abuffersink filter.\n");
        return AVERROR_FILTER_NOT_FOUND;
    }

    abuffersink_ctx = avfilter_graph_alloc_filter(filter_graph, abuffersink, "sink");
    if (!abuffersink_ctx)
    {
        fprintf(stderr, "Could not allocate the abuffersink instance.\n");
        return AVERROR(ENOMEM);
    }

    /* This filter takes no options. */
    err = avfilter_init_str(abuffersink_ctx, NULL);
    if (err < 0)
    {
        fprintf(stderr, "Could not initialize the abuffersink instance.\n");
        return err;
    }

    /* Connect the filters;
     * in this simple case the filters just form a linear chain. */
    err = avfilter_link(abuffer_ctx, 0, volume_ctx, 0);
    if (err >= 0)
        err = avfilter_link(volume_ctx, 0, aformat_ctx, 0);
    if (err >= 0)
        err = avfilter_link(aformat_ctx, 0, abuffersink_ctx, 0);
    if (err < 0)
    {
        fprintf(stderr, "Error connecting filters\n");
        return err;
    }

    /* Configure the graph. */
    err = avfilter_graph_config(filter_graph, NULL);
    if (err < 0)
    {
        av_log(NULL, AV_LOG_ERROR, "Error configuring the filter graph\n");
        return err;
    }
    av_buffersink_set_frame_size(abuffersink_ctx, encctx->frame_size);
    *graph = filter_graph;
    *src = abuffer_ctx;
    *sink = abuffersink_ctx;

    return 0;
}

static void logging(const char *fmt, ...)
{
    va_list args;
    fprintf(stderr, "LOG: ");
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fprintf(stderr, "\n");
}

int encode(AVFormatContext *avfc, AVStream *dec_str, AVStream *enc_str, AVCodecContext *avcc, int index, AVFrame *input_frame)
{
    AVPacket *output_packet = av_packet_alloc();
    if (!output_packet)
    {
        logging("could not allocate memory for output packet");
        return -1;
    }

    int response = avcodec_send_frame(avcc, input_frame);

    while (response >= 0)
    {
        response = avcodec_receive_packet(avcc, output_packet);
        if (response == AVERROR(EAGAIN) || response == AVERROR_EOF)
        {
            break;
        }
        else if (response < 0)
        {
            logging("Error while receiving packet from encoder: %s", av_err2str(response));
            return -1;
        }
        output_packet->stream_index = index;

        av_packet_rescale_ts(output_packet, dec_str->time_base, enc_str->time_base);
        response = av_interleaved_write_frame(avfc, output_packet);
        if (response != 0)
        {
            logging("Error %d while receiving packet from decoder: %s", response, av_err2str(response));
            return -1;
        }
    }
    av_packet_unref(output_packet);
    av_packet_free(&output_packet);
    return 0;
}

int main(int argc, char **argv)
{

    AVFormatContext *ctx = nullptr;
    AVStream *istream = nullptr;
    AVCodecContext *encctx = nullptr;
    AVStream *ostream;
    AVFilterGraph *graph;
    AVFilterContext *src, *sink;
    std::string path("https://path.to.m3u8");
    int streamIdx;
    int packetsProcessed;
    int maxPackets = 2000;

    av_log_set_level(AV_LOG_DEBUG);
    av_log_set_callback(av_log_default_callback);

    ctx = avformat_alloc_context();

    if (avformat_open_input(&ctx, path.c_str(), 0, 0) < 0)
        std::cout << "error1" << std::endl;

    if (avformat_find_stream_info(ctx, 0) < 0)
        std::cout << "error2" << std::endl;

    int status;

    AVFormatContext *ofmt_ctx = nullptr;
    const AVOutputFormat *ofmt = av_guess_format("adts", NULL, NULL);

    std::cout << "test!" << std::endl;
    status = avformat_alloc_output_context2(&ofmt_ctx, ofmt, "adts", NULL);

    if (status < 0)
    {
        std::cerr << "could not allocate output format" << std::endl;
        return 0;
    }

    streamIdx = av_find_best_stream(ctx, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
    istream = ctx->streams[streamIdx];
    av_channel_layout_default(&istream->codecpar->ch_layout, 2);
    const AVCodec *dec = avcodec_find_decoder(istream->codecpar->codec_id);
    AVCodecContext *decctx;
    decctx = avcodec_alloc_context3(dec);
    if (!decctx)
    {
        std::cerr << "failed to alloc memory for codec context" << std::endl;
        return -1;
    }
    status = avcodec_parameters_to_context(decctx, istream->codecpar);
    if (status < 0)
    {
        avcodec_free_context(&decctx);
        std::cerr << "could not set decoder params" << std::endl;
        return 0;
    }

    avcodec_open2(decctx, dec, NULL);
    if (status < 0)
    {
        avcodec_free_context(&decctx);
        std::cerr << "could not open a decoder" << std::endl;
        return 0;
    }

    int audio_stream_index = 0;
    for (unsigned i = 0; i < ctx->nb_streams; i++)
    {
        if (ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO)
        {
            audio_stream_index = i;
            std::cout << "looking for appropriate encoder" << std::endl;
            const AVCodec *c = avcodec_find_encoder(decctx->codec->id);
            encctx = avcodec_alloc_context3(c);
            if (c)
            {
                encctx->codec_type = AVMEDIA_TYPE_AUDIO;
                av_channel_layout_copy(&encctx->ch_layout, &decctx->ch_layout);
                encctx->time_base.num = 1;
                encctx->time_base.den = decctx->sample_rate;
                encctx->bit_rate = decctx->bit_rate;
                encctx->sample_rate = decctx->sample_rate;
                encctx->sample_fmt = decctx->sample_fmt;
                ostream = avformat_new_stream(ofmt_ctx, c);

                if (avcodec_open2(encctx, c, NULL) < 0)
                {
                    logging("failed to open codec through avcodec_open2");
                    return -1;
                }

                if (avcodec_parameters_from_context(ostream->codecpar, encctx) < 0)
                {
                    logging("failed to copy parameters to output stream");
                    return -1;
                }

                /* Set up the filtergraph. */
                status = init_filter_graph(&graph, &src, &sink, encctx, decctx);
                if (status < 0)
                {
                    fprintf(stderr, "Unable to init filter graph:");
                    return 0;
                }

                std::cerr << "filters created successfully" << std::endl;
            }
            break;
        }
    }

    av_dump_format(ofmt_ctx, 0, "./nice.aac", 1);

    if (!(ofmt_ctx->oformat->flags & AVFMT_NOFILE))
    {
        avio_open2(&ofmt_ctx->pb, "./nice.aac", AVIO_FLAG_WRITE, NULL, NULL);
    }
    ofmt_ctx->flags |= AVFMT_FLAG_BITEXACT;

    // Copy metadata from source
    av_dict_copy(&ofmt_ctx->metadata, ctx->metadata, AV_DICT_APPEND);
    av_dict_set(&ofmt_ctx->metadata, "ISRC", "yoursuperfabulousisrccodegoeshere", AV_DICT_APPEND);

    AVDictionary *options;
    status = avformat_write_header(ofmt_ctx, &options);

    AVFrame *input_frame = av_frame_alloc();
    AVFrame *out_frame = av_frame_alloc();
    AVPacket *input_packet = av_packet_alloc();
    int c;
    while (packetsProcessed < maxPackets && av_read_frame(ctx, input_packet) == 0)
    {
        packetsProcessed++;
        printf(
                    "Incoming packet received (time base den:%d) pts %d dts %d duration in time base units %d [pkt position %d, size %d]\n",
                    input_packet->time_base.den,
                    input_packet->pts,
                    input_packet->dts,
                    input_packet->duration,
                    input_packet->pos,
                    input_packet->size);
        c++;
        int response = avcodec_send_packet(decctx, input_packet);
        while (response >= 0)
        {
            response = avcodec_receive_frame(decctx, input_frame);
            if (response == AVERROR(EAGAIN) || response == AVERROR_EOF)
            {
                break;
            }
            else if (response < 0)
            {
                return response;
            }
            if (response >= 0)
            {
                printf(
                    "Incoming frame %c (nb_samples:%d) pts %d dts %d key_frame %d [pkt position %d, sample rate %d]\n",
                    av_get_picture_type_char(input_frame->pict_type),
                    input_frame->nb_samples,
                    input_frame->pts,
                    input_frame->pkt_dts,
                    input_frame->key_frame,
                    input_frame->pkt_pos,
                    input_frame->sample_rate);
                response = av_buffersrc_add_frame(src, input_frame);
                if (response < 0)
                {
                    fprintf(stderr, "Cannot send frame to afilter:");
                    break;
                }
                while ((response = av_buffersink_get_frame(sink, out_frame)) >= 0)
                {
                    encode(ofmt_ctx, istream, ostream, encctx, audio_stream_index, out_frame);
                }
            }
            av_frame_unref(input_frame);
            av_frame_unref(out_frame);
        }
        av_packet_unref(input_packet);
    }

    av_interleaved_write_frame(ofmt_ctx, NULL);

    av_dump_format(ofmt_ctx, 0, "./nice.aac", 1);
    av_dict_free(&options);

    av_packet_free(&input_packet);
    av_frame_free(&input_frame);
    av_frame_free(&out_frame);

    av_write_trailer(ofmt_ctx);

    avformat_close_input(&ctx);
    avformat_free_context(ofmt_ctx);
    avformat_free_context(ctx);
    avcodec_free_context(&decctx);
    avcodec_free_context(&encctx);
    avfilter_graph_free(&graph);

    if (status == 0)
        std::cout << "test1" << std::endl;

    return 0;
}
