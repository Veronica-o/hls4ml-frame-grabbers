//
//    rfnoc-hls-neuralnet: Vivado HLS code for neural-net building blocks
//
//    Copyright (C) 2017 EJ Kreinar
//
//    This program is free software: you can redistribute it and/or modify
//    it under the terms of the GNU General Public License as published by
//    the Free Software Foundation, either version 3 of the License, or
//    (at your option) any later version.
//
//    This program is distributed in the hope that it will be useful,
//    but WITHOUT ANY WARRANTY; without even the implied warranty of
//    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//    GNU General Public License for more details.
//
//    You should have received a copy of the GNU General Public License
//    along with this program.  If not, see <http://www.gnu.org/licenses/>.
//
#include <iostream>

#include "myproject.h"
#include "parameters.h"


// Unpacks image data and crops image. Forwards ROI to model
void unpack_data(hls::stream<input_arr_t> (&input_arr_split_reordered)[NUM_STRIPES], hls::stream<input_t> &input_1){

  assert(((CROP_START_X >= 0) && (CROP_START_X <= IMAGE_WIDTH) && ((CROP_START_X + CROP_WIDTH) <= IMAGE_WIDTH) && (CROP_START_Y >= 0) && (CROP_START_Y <= IMAGE_HEIGHT) && ((CROP_START_Y + CROP_HEIGHT) <= IMAGE_HEIGHT)) && "CustomLogic: Your crop region must be inside the image!");
  
  unsigned curr_X = 0; // Current x position relative to raw image
  unsigned curr_Y = 0; // Current y position relative to raw image
  input_t temp_in; // NN input sample

  // #ifndef __SYNTHESIS__
  //   std::cout << "\n\nCropped Normalized Model Input: \n";
  // #endif

  InputSplitLoop: // Split input into stripe arrangement, this is inherent to the camera readout
  for(unsigned i = 0; i < PACKETS_PER_IMAGE; i++){
    #pragma HLS PIPELINE
    input_arr_t temp_arr_in = input_arr_split_reordered[curr_Y / STRIPE_HEIGHT].read(); // Read CoaxPress packet

    // Check if current packet holds any portion of the desired crop region
    if(((curr_X + MONO8PIX_NBR) >= CROP_START_X) && (curr_X < (CROP_START_X + CROP_WIDTH)) && (curr_Y >= CROP_START_Y) && (curr_Y < (CROP_START_Y + CROP_HEIGHT))){

      unsigned rel_START_X; // Relative x start position within current packet which holds part of the desired crop region
      unsigned rel_END_X;   // Relative x end position

      if(curr_X < CROP_START_X){
        rel_START_X = CROP_START_X - curr_X;
      }else{
        rel_START_X = 0;
      }

      if((curr_X + MONO8PIX_NBR) > (CROP_START_X + CROP_WIDTH)){
        rel_END_X = (CROP_START_X + CROP_WIDTH) - curr_X;
      }else{
        rel_END_X = MONO8PIX_NBR;
      }

      // Unpack pixels and write to the NN input stream
      UnpackLoop:
      for(int j = rel_START_X; j < rel_END_X; j++){
        #pragma HLS UNROLL
        temp_in[0] = temp_arr_in[j];
        input_1.write(temp_in);

        // #ifndef __SYNTHESIS__
        //   std::cout << temp_arr_in[j] << " ";
        // #endif
      }
    }

    // Track position in frame
    if(curr_X == (IMAGE_WIDTH - MONO8PIX_NBR)){
      curr_X = 0;
      curr_Y++;
    }else{
      curr_X = curr_X + MONO8PIX_NBR;
    }
  }
}


// Reads and duplicates CoaxPress image data into buffer and model input stream 
void read_pixel_data(hls::stream<video_if_t> &VideoIn, hls::stream<video_if_t> &VideoBuffer, hls::stream<input_arr_t> (&input_arr_split_reordered)[NUM_STRIPES]){

  // Whether or not we're in the frame
  bool inFrame;

  static video_if_t DataBuf;
  //Latch meta data as received as input image is identical
  //in size and format to output image

  unsigned curr_Y = 0; // Current y position relative to raw image

  //Reset input control signal
  DataBuf.User = 0;
  FrameLoop:
  do {
    #pragma HLS PIPELINE
    bool InLine;

    //As long as the frame is not finish
    if (DataBuf.nEndOF) {
      //Read data
      VideoIn >> DataBuf; // Same as DataBuf = VideoIn.read()
      inFrame = true;
    } else{ // If frame has finished
      //If End of frame met, reset inFrame flag
      inFrame = false;
      // And forward EOF if not sync with EOL
      if (DataBuf.nEndOL) {
        // Forward EOF if not sync with EOL
        video_if_t output_buf;
        output_buf.Data = 0;
        output_buf.User = DataBuf.User;
        VideoBuffer << output_buf;
      }
    }
    // If Start of frame is detected without Start Of Line
    if (DataBuf.SOF && DataBuf.nSOL) {
      // Forward SOF if not sync with SOL
      VideoBuffer << DataBuf;
    }
    // If Line Start, do computation
    if (DataBuf.SOL) {

      WriteInLoop:
      do{
        #pragma HLS PIPELINE

        video_if_t output_buf;
        input_arr_t ctype;

        Process_pixel:
        for (unsigned char i = 0; i < MONO8PIX_NBR; i++) {
          output_buf.MONO8PIX(i) = DataBuf.MONO8PIX(i);
          ctype[i] = ((ap_ufixed<32,pixMono8::width>)DataBuf.MONO8PIX(i)) / NORM_DIV; //Normalize pixel values and set input to model // TODO: Add functions for different methods of scaling.
        }

				// Set output control signal
				output_buf.User = DataBuf.User;
				//Store the result in the output stream
				VideoBuffer << output_buf;

        // Reorder image for input to model and write to output
        input_arr_split_reordered[stripe_order[curr_Y / STRIPE_HEIGHT]].write(ctype);

        // If line is not finish
        if (DataBuf.nEndOL) {
          //Keep reading
          InLine = true;
          VideoIn >> DataBuf;
        } else {
          InLine = false;
        }
      } while (InLine);
      
      curr_Y++;
    }
  } while (inFrame);
}


// Insert neural network predictions to head of image output.
void attach_results(hls::stream<result_t> &layer_out, hls::stream<video_if_t> &VideoBuffer, hls::stream<video_if_t> &VideoOut, result_t::value_type &ModelOutLast){

  #ifndef __SYNTHESIS__
    std::ofstream fout("../../../../tb_data/csim_results.log", std::ios::app);
  #endif

  unsigned output_count = 0;

  static video_if_t DataOut;
  video_if_t output_buf;

  result_t temp;

  unsigned bits_left = STREAM_DATA_WIDTH;
  unsigned start_idx = result_t::value_type::width * result_t::size;
  bool overlap = false;

  for(unsigned i = 0; i < (IMAGE_HEIGHT * IMAGE_WIDTH) / MONO8PIX_NBR; i++){
    #pragma HLS PIPELINE

    VideoBuffer >> DataOut;


    bits_left = STREAM_DATA_WIDTH;
  
    output_buf.User = DataOut.User;
    output_buf.Data = DataOut.Data;

    while(!layer_out.empty()){
      
      // Read new sample if there are no more bits to write
      if(!overlap){
        temp = layer_out.read();

        #ifndef __SYNTHESIS__
          for(unsigned i = 0; i < result_t::size; i++){
            std::cout << temp[i] << " ";
            fout << temp[i] << " ";
          }
        #endif

        if(layer_out.empty()){
          ModelOutLast = temp[0]; // Output last model output to signal inference completion
        }

        output_count++;

        assert(((output_count * result_t::size * result_t::value_type::width) < (IMAGE_HEIGHT * IMAGE_WIDTH * pixMono8::width)) && "CustomLogic: Your model output is too large!! The total size of your model output (in bits) is larger than the image."); // Check that size (in bits) of predictions are less than size of full image

        start_idx = result_t::value_type::width * result_t::size;
      }

      if(bits_left >= start_idx){ // If there is room to fit the entire model output in the current stream packet
        for(unsigned j = ((result_t::size-1) - ((start_idx-1) / result_t::value_type::width)); j < result_t::size; j++){
          #pragma HLS UNROLL
          if(j==0){
            output_buf.Data = (output_buf.Data << (((start_idx-1) % result_t::value_type::width)+1)) | temp[j].range((start_idx-1) % result_t::value_type::width, 0);
          }else{
            output_buf.Data = (output_buf.Data << (result_t::value_type::width)) | temp[j].range(result_t::value_type::width-1, 0);
          }
        }

        bits_left = bits_left - start_idx;
        overlap = false;
      }else{ // only bits_left bits left in current stream packet, but more bits in current model output

        for(unsigned j = 0; j < (((result_t::value_type::width * result_t::size) - bits_left) / result_t::value_type::width)+1; j++){
          #pragma HLS UNROLL
          if(j == (((result_t::value_type::width * result_t::size) - bits_left) / result_t::value_type::width)){
            output_buf.Data = (output_buf.Data << (bits_left % result_t::value_type::width)) | temp[j].range(result_t::value_type::width - 1, ((result_t::value_type::width * result_t::size) - bits_left) % result_t::value_type::width);
          }else{
            output_buf.Data = (output_buf.Data << result_t::value_type::width) | temp[j].range(result_t::value_type::width-1, 0);
          }
        }

        // start_idx = (result_t::value_type::width * result_t::size) - bits_left;
        start_idx = start_idx - bits_left;
        overlap = true;
        break;
      }
    }
    VideoOut << output_buf;
  }

  #ifndef __SYNTHESIS__
    std::cout << "\n";
    fout << "\n";
    fout.close();
  #endif
}


void myproject(
    hls::stream<video_if_t> &VideoIn, 
    hls::stream<video_if_t> &VideoOut,
    result_t::value_type &ModelOutLast // Output a single bit so we can monitor inference latency
) {

    assert((((IMAGE_HEIGHT * IMAGE_WIDTH * pixMono8::width) % STREAM_DATA_WIDTH) == 0) && "CustomLogic: Your image size (in bits) must be a multiple of the stream depth (Octo: 128, Quad CXP-12: 256)");
    
    assert(((IMAGE_WIDTH % MONO8PIX_NBR) == 0) && "CustomLogic: Your image width (in pixels) must be a multiple of the stream depth (in pixels, see value of MONO8PIX_NBR)");
    
    assert(((IMAGE_HEIGHT % BLOCK_HEIGHT) == 0) && "CustomLogic: Block height must be a multiple of the image height");

    //hls-fpga-machine-learning insert IO
    #pragma HLS DATAFLOW 

    unsigned PACKED_DEPTH =  ((IMAGE_WIDTH * IMAGE_HEIGHT) / MONO8PIX_NBR);
    unsigned UNPACKED_DEPTH = (IMAGE_WIDTH * IMAGE_HEIGHT);



/////////////////////////////////////////////////////////////////////////////////////////////////////////////
    

/* CustomLogic: INSERT MODEL WEIGHT LOAD STATEMENTS HERE */


/////////////////////////////////////////////////////////////////////////////////////////////////////////////

    //hls-fpga-machine-learning insert layers
    hls::stream<input_arr_t> input_arr_split_reordered[NUM_STRIPES];
    for(unsigned i = 0; i < NUM_STRIPES; i++){
      #pragma HLS STREAM variable=input_arr_split_reordered[i] depth=PACKETS_PER_STRIPE
    }

    hls::stream<video_if_t> VideoBuffer; // Holds buffered unaltered image
    #pragma HLS STREAM variable=VideoBuffer depth=PACKED_DEPTH

    read_pixel_data(VideoIn, VideoBuffer, input_arr_split_reordered); // Read CoaXPress image data 

    hls::stream<input_t> input_1("input_1"); // Holds cropped image input to neural network
    #pragma HLS STREAM variable=input_1 depth=UNPACKED_DEPTH

    unpack_data(input_arr_split_reordered, input_1); // Crop image by unpacking only pixels which serve as input to model

/////////////////////////////////////////////////////////////////////////////////////////////////////////////

    /* CustomLogic: INSESRT NEURAL NETWORK LAYERS HERE */

/////////////////////////////////////////////////////////////////////////////////////////////////////////////

    attach_results( /* CustomLogic: INSERT LAST NETWORK LAYER OUTPUT STREAM HERE */ , VideoBuffer, VideoOut, ModelOutLast); // Attach neural network predictions to image output
}