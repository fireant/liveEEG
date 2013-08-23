/*Copyright (c) 2013, Mosalam Ebrahimi
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this
   list of conditions and the following disclaimer.
2. Redistributions in binary form must reproduce the above copyright notice,
   this list of conditions and the following disclaimer in the documentation
   and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR
ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.*/


#ifndef FFT_H
#define FFT_H

#include <fstream> // to read multitaper files
#include <boost/circular_buffer.hpp> // to hold recent samples
#include <fftw3.h> // fft library
//#include <omp.h>
#define _USE_MATH_DEFINES // pi
#include <cmath>

template <class T>
class Fft{
public:
  enum windowFunc {NONE=0, HAMMING, BLACKMAN_HARRIS, MULTITAPER};
  Fft(size_t winSize, windowFunc winf_, size_t frq_, size_t numChannels=1,
      size_t numTapers=5);
  ~Fft();
  void AddPoints(std::vector<T>& p);
  bool Process();
  void GetPower(std::vector<std::vector<T> >& pow);
  void GetPhase(std::vector<std::vector<T> >& pha, std::vector<T>* phaseShift=NULL);
private:
  std::vector<boost::circular_buffer<T> > buffer;
  fftw_complex* out;
  std::vector<fftw_complex*> outVec;
  T* in;
  T* inTmp;
  T* winFunc;
  fftw_plan plan_forward;
  windowFunc winf;
  double winFuncSum;
  size_t frq;
  std::vector<T*> tapers;
  std::vector<T> tapersWeights;
  static const std::string dpssVectors;
  static const std::string dpssValues;
  size_t numTapers;
};

template<class T>
const std::string Fft<T>::dpssVectors = "dpss_E_256_5";
template<class T>
const std::string Fft<T>::dpssValues = "dpss_V_256_5";

template<class T>
Fft<T>::Fft(size_t winSize, windowFunc winf_, size_t frq_, size_t numChannels,
            size_t numTapers) :
  winf(winf_), frq(frq_) {
  //fftw_init_threads();
  //fftw_plan_with_nthreads(omp_get_max_threads());
  out = (fftw_complex *)fftw_malloc(sizeof(fftw_complex) * ((winSize/2)+1) );
  in = (T*)fftw_malloc(sizeof(T) * winSize);
  inTmp = (T*)fftw_malloc(sizeof(T) * winSize);
  // FFTW_MEASURE: slow initialization, fast run time
  // FFTW_ESTIMATE: fast initialization, slow run time
  plan_forward = fftw_plan_dft_r2c_1d(winSize, in, out, FFTW_MEASURE);
  if (winf_ == windowFunc::MULTITAPER) {
    std::ifstream vecFile(dpssVectors);

    for (size_t i=0; i<numTapers; i++)
      tapers.push_back((T*)fftw_malloc(sizeof(T) * winSize));
    T val;
    size_t colNum = 0;
    bool eof = false;
    for (;!eof;) {
        std::cout<<"e "<<std::endl;
      for (size_t i=0; i<numTapers; i++) {
        vecFile>>val;
        if (!vecFile.good()) {
          eof = true;
          break;
        }
        tapers[i][colNum] = val;
        std::cout<<"t "<<i<<"  v:"<<val<<std::endl;
      }
      colNum += 1;
    }
    std::ifstream valFile(dpssValues);
    for (size_t i=0; i<numTapers; i++) {
      valFile>>val;
      tapersWeights.push_back(val);
      std::cout<<"t "<<i<<"  v:"<<val<<std::endl;
    }
    winFuncSum = 1.0/2.0;
  } else if (winf_ == windowFunc::HAMMING) {
    winFunc = (double *)fftw_malloc ( sizeof ( double ) * winSize );
    winFuncSum = 0.0;
    for (size_t i=0; i<winSize; i++) {
      winFunc[i] = 0.54 - 0.46 * cos(2.0*M_PI* float(i)/ (winSize - 1.0));
      winFuncSum += winFunc[i];
    }
    winFuncSum = 2.0/winFuncSum;
  } else  if (winf_ == windowFunc::BLACKMAN_HARRIS) {
    winFunc = (double *)fftw_malloc ( sizeof ( double ) * winSize );
    winFuncSum = 0.0;
    for (size_t i=0; i<winSize; i++) {
      winFunc[i] = 0.35875 - 0.48829 * cos(2.0*M_PI* float(i)/(winSize-1.0))
                           + 0.14128 * cos(4.0*M_PI* float(i)/(winSize-1.0))
                           - 0.01168 * cos(6.0*M_PI* float(i)/(winSize-1.0));
      winFuncSum += winFunc[i];
    }
    winFuncSum = 2.0/winFuncSum;
  } else {
    winFuncSum = 2.0/(winSize+2);
  }
  outVec.resize(numChannels);
  buffer.resize(numChannels);
  for (size_t i=0; i<buffer.size(); i++) {
    buffer[i].set_capacity(winSize);
    outVec[i] = (fftw_complex *)fftw_malloc(sizeof(fftw_complex)*(winSize/2+1));
  }
}

template<class T>
Fft<T>::~Fft() {
  //fftw_cleanup_threads();
  fftw_free(out);
  fftw_free(in);
  fftw_free(inTmp);
  if (winf != windowFunc::NONE)
    fftw_free(winFunc);
}

template<class T>
void Fft<T>::AddPoints(std::vector<T>& p) {
  for (size_t i=0; i<p.size(); i++) {
    buffer[i].push_back(p[i]);
  }
}

template<class T>
bool Fft<T>::Process() {
  if (buffer[0].size() < buffer[0].capacity())
    return false;

  if (winf == windowFunc::MULTITAPER) {
    for (size_t sig=0; sig<buffer.size(); sig++) {
      memcpy(inTmp, buffer[sig].linearize(), sizeof(T)*buffer[sig].capacity());

      const size_t nc = buffer[sig].capacity()/2+1;
      std::vector<T> outTmpRe(nc);
      std::vector<T> outTmpIm(nc);
      for (size_t j = 0; j < nc; j++) {
        outTmpRe[j] = outTmpIm[j] = 0.0;
      }
      for (size_t i=0; i<tapers.size(); i++){
        for (size_t j=0; j<buffer[sig].capacity(); j++)
          in[j] = inTmp[j] * tapers[i][j];
        fftw_execute(plan_forward);
        for (size_t j = 0; j < nc; j++) {
          outTmpRe[j] += out[j][0]*tapersWeights[i]/T(tapers.size());
          outTmpIm[j] += out[j][1]*tapersWeights[i]/T(tapers.size());
        }
      }
      for (size_t i = 0; i < nc; i++) {
        out[i][0] = outTmpRe[i];
        out[i][1] = outTmpIm[i];
      }
      memcpy(outVec[sig], out, sizeof(fftw_complex)*nc);
    }
  } else if (winf != windowFunc::NONE) {
    for (size_t sig=0; sig<buffer.size(); sig++) {
      const size_t nc = buffer[sig].capacity()/2+1;
      memcpy(in, buffer[sig].linearize(), sizeof(T)*buffer[sig].capacity());
      for (size_t i=0; i<buffer[sig].capacity(); i++)
        in[i] *= winFunc[i];
      fftw_execute(plan_forward);
      memcpy(outVec[sig], out, sizeof(fftw_complex)*nc);
    }
  } else {
    for (size_t sig=0; sig<buffer.size(); sig++) {
      const size_t nc = buffer[sig].capacity()/2+1;
      memcpy(in, buffer[sig].linearize(), sizeof(T)*buffer[sig].capacity());
      fftw_execute(plan_forward);
      memcpy(outVec[sig], out, sizeof(fftw_complex)*nc);
    }
  }

  return true;
}


template<class T>
void Fft<T>::GetPower(std::vector<std::vector<T> >& pow) {
  const size_t nc = buffer[0].capacity()/2+1;
  for (size_t sig=0; sig<buffer.size(); sig++) {
    for (size_t i = 0; i < nc; i++) {
      pow[sig][i] = sqrt(outVec[sig][i][0]*outVec[sig][i][0] + outVec[sig][i][1]*outVec[sig][i][1]);
      pow[sig][i] *= winFuncSum;
    }
  }
}

template<class T>
void Fft<T>::GetPhase(std::vector<std::vector<T> >& pha,
                      std::vector<T>* phaseShift) {
  const size_t nc = buffer[0].capacity()/2+1;
  for (size_t sig=0; sig<buffer.size(); sig++) {
    for (size_t i = 0; i < nc; i++ ) {
      pha[sig][i] = atan2(outVec[sig][i][1], outVec[sig][i][0]);

      if (phaseShift && sig==0) {
        float por = float(i)/float(nc-1);
        (*phaseShift)[i] = por*M_PI;
      }
    }
  }
}

//double GetFrqOfBin(size_t i) {
//  const size_t nc = buffer[0].capacity()/2+1;
//  float por = float(i)/float(nc-1);
//}

#endif // FFT_H
