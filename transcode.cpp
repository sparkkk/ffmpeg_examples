#include <stdio.h>
#include <string>
#include <vector>


extern "C"
{
#include "libavcodec/avcodec.h"
#include "libavformat/avformat.h"
#include "libavutil/error.h"
#include "libavutil/imgutils.h"
#include "libswscale/swscale.h"
}

static void print_error(int av_result)
{
    char buf[1024];
    av_strerror(av_result, buf, sizeof(buf));
    printf("error: %s\n", buf);
}

int run(const std::string & in_path, const std::string & out_path)
{
    int width = 4;
    int height = 4;
    int av_result = 0;
    AVFormatContext * fmt_ctx_input = NULL;
    avformat_open_input(&fmt_ctx_input, in_path.c_str(), NULL, NULL);
    avformat_find_stream_info(fmt_ctx_input, NULL);
    AVStream * stream_in = fmt_ctx_input->streams[0];
    AVCodecContext * codec_ctx_input = avcodec_alloc_context3(NULL);
    avcodec_parameters_to_context(codec_ctx_input, stream_in->codecpar);
    codec_ctx_input->framerate = av_guess_frame_rate(fmt_ctx_input, stream_in, NULL);
    width = codec_ctx_input->width;
    height = codec_ctx_input->height;
    const AVCodec * codec_input = avcodec_find_decoder(codec_ctx_input->codec_id);
    av_result = avcodec_open2(codec_ctx_input, codec_input, NULL);
    if (av_result != 0) {
        printf("avcodec_open2 failed for input\n");
        print_error(av_result);
    }
    //av_dump_format(fmt_ctx_input, 0, NULL, 0);

    printf("size = %dx%d\n", width, height);

    AVFormatContext * fmt_ctx_output = NULL;
    av_result = avformat_alloc_output_context2(&fmt_ctx_output, NULL, NULL, out_path.c_str());
    if (av_result != 0) {
        print_error(av_result);
    }
    AVStream * stream_out = avformat_new_stream(fmt_ctx_output, NULL);
    const AVCodec * codec_output = avcodec_find_encoder(AV_CODEC_ID_H264);
    AVCodecContext * codec_ctx_output = avcodec_alloc_context3(codec_output);
    codec_ctx_output->width = width;
    codec_ctx_output->height = height;
    codec_ctx_output->bit_rate = codec_ctx_input->bit_rate;
    codec_ctx_output->sample_aspect_ratio = codec_ctx_input->sample_aspect_ratio;
    codec_ctx_output->pix_fmt = AVPixelFormat::AV_PIX_FMT_YUV420P;
    codec_ctx_output->time_base = stream_in->time_base;
    codec_ctx_output->max_b_frames = 1;
    if (fmt_ctx_output->oformat->flags & AVFMT_GLOBALHEADER) {
        codec_ctx_output->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }
    AVDictionary * codec_options = NULL;
    av_dict_set(&codec_options, "preset", "fast", 0);
    //not know why but if x264 mode was not crf (by default it is arf), 
    //the output video might start with some grey frames.
    //the first frame of encoded video being key-frame and I-pic ensured.
    av_dict_set(&codec_options, "x264opts", "crf=23", 0);
    av_result = avcodec_open2(codec_ctx_output, NULL, &codec_options);
    if (av_result != 0) {
        printf("avcodec_open2 failed for output\n");
        print_error(av_result);
    }
    av_dict_free(&codec_options);
    avcodec_parameters_from_context(stream_out->codecpar, codec_ctx_output);
    stream_out->time_base = codec_ctx_output->time_base;

    //only transcode the video stream

    av_result = avio_open(&fmt_ctx_output->pb, out_path.c_str(), AVIO_FLAG_WRITE);
    if (av_result != 0) {
        print_error(av_result);
    }

    av_result = avformat_write_header(fmt_ctx_output, NULL);
    if (av_result != 0) {
        print_error(av_result);
    }
    //av_dump_format(fmt_ctx_output, 0, NULL, 1);

    AVPacket * packet_in = av_packet_alloc();
    AVPacket * packet_out = av_packet_alloc();
    AVFrame * frame_in = av_frame_alloc();
    AVFrame * frame_out = av_frame_alloc();

    frame_out->width = width;
    frame_out->height = height;
    frame_out->format = AV_PIX_FMT_YUV420P;
    av_frame_get_buffer(frame_out, 0);
    
    int frame_cnt = 0;
    while (packet_in != NULL) {
        av_result = av_read_frame(fmt_ctx_input, packet_in);
        if (av_result == AVERROR_EOF) {
            av_packet_free(&packet_in);
            //then flush input codec by calling avcodec_send_packet
            //with packet_in == NULL
        } else if (av_result != 0) {
            break;
        } else if (packet_in->stream_index != 0) {
            //assumed the input video stream index is 0
            //only transcode the video stream
            continue;
        }
        av_result = avcodec_send_packet(codec_ctx_input, packet_in);
        if (av_result != 0) {
            break;
        }
        while (frame_in != NULL) {
            av_result = avcodec_receive_frame(codec_ctx_input, frame_in);
            if (av_result == AVERROR_EOF) {
                av_frame_free(&frame_in);
                av_frame_free(&frame_out);
                //then flush output codec by calling avcodec_send_frame
                //with frame_out == NULL
            } else if (av_result != 0) {
                break;
            } else {
                av_result = av_frame_copy(frame_out, frame_in);
                if (frame_in->nb_side_data > 0) {
                    AVFrameSideData * side_data = av_frame_get_side_data(
                        frame_in, 
                        AVFrameSideDataType::AV_FRAME_DATA_SEI_UNREGISTERED
                    );
                    av_frame_new_side_data_from_buf(
                        frame_out,
                        AVFrameSideDataType::AV_FRAME_DATA_SEI_UNREGISTERED,
                        side_data->buf
                    );
                }
                frame_out->pts = frame_in->pts;
                //scale frame pts if time-bases are different between input and
                //output codec context
                ++frame_cnt;
            }

            av_result = avcodec_send_frame(codec_ctx_output, frame_out);
            if (av_result != 0) {
                break;
            }
            while (packet_out != NULL) {
                av_result = avcodec_receive_packet(codec_ctx_output, packet_out);
                if (av_result == AVERROR_EOF) {
                    av_packet_free(&packet_out);
                    //then flush output steam by calling av_interleaved_write_frame
                    //with packet_out == NULL
                } else if (av_result != 0) {
                    break;
                } else {
                    packet_out->stream_index = stream_out->index;
                    //scale packet pts if time-bases are different between output
                    //codec context and output stream
                }
                av_result = av_interleaved_write_frame(fmt_ctx_output, packet_out);
                if (av_result != 0) {
                    break;
                }
            }
        }
    }
    if (av_result != 0) {
        print_error(av_result);
    }
    if (frame_in != NULL) {
        av_frame_free(&frame_in);
    }
    if (frame_out != NULL) {
        av_frame_free(&frame_out);
    }
    if (packet_in != NULL) {
        av_packet_free(&packet_in);
    }
    if (packet_out != NULL) {
        av_packet_free(&packet_out);
    }

    av_result = av_write_trailer(fmt_ctx_output);
    if (av_result != 0) {
        print_error(av_result);
    }

    avcodec_close(codec_ctx_output);
    avio_close(fmt_ctx_output->pb);
    avformat_free_context(fmt_ctx_output);

    avcodec_close(codec_ctx_input);
    avformat_close_input(&fmt_ctx_input);
    printf("finished\n");
    return 0;

}

int main(int argc, const char * argv[])
{
    int r = run("in.mp4", "out.mp4");
    {
        printf("Enter to exit");
        int a = 0;
        fread(&a, 1, 1, stdin);
    }
    return r;
}