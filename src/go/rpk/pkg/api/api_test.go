// Copyright 2020 Redpanda Data, Inc.
//
// Use of this software is governed by the Business Source License
// included in the file licenses/BSL.md
//
// As of the Change Date specified in that file, in accordance with
// the Business Source License, use of this software will be governed
// by the Apache License, Version 2.0

package api

import (
	"encoding/json"
	"io"
	"net/http"
	"net/http/httptest"
	"testing"
	"time"

	"github.com/redpanda-data/redpanda/src/go/rpk/pkg/config"
	"github.com/stretchr/testify/require"
)

func TestSendMetrics(t *testing.T) {
	body := metricsBody{
		NodeUUID:     "asdfas-asdf2w23sd-907asdf",
		Organization: "io.vectorized",
		NodeID:       1,
		SentAt:       time.Now(),
		MetricsPayload: MetricsPayload{
			FreeMemoryMB:  100,
			FreeSpaceMB:   200,
			CPUPercentage: 89,
		},
	}
	bs, err := json.Marshal(body)
	require.NoError(t, err)

	ts := httptest.NewServer(
		http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
			b, err := io.ReadAll(r.Body)
			require.NoError(t, err)
			require.Exactly(t, bs, b)
			w.WriteHeader(http.StatusOK)
		}))
	defer ts.Close()

	conf := config.Default()
	conf.Rpk.EnableUsageStats = true
	err = sendMetricsToURL(body, ts.URL, *conf)
	require.NoError(t, err)
}

func TestSkipSendMetrics(t *testing.T) {
	ts := httptest.NewServer(
		http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
			require.FailNow(
				t,
				"The request shouldn't have been sent if metrics collection is disabled",
			)
		}),
	)
	defer ts.Close()

	conf := config.Default()
	conf.Rpk.EnableUsageStats = false
	err := sendMetricsToURL(metricsBody{}, ts.URL, *conf)
	require.NoError(t, err)
}

func TestSendEnvironment(t *testing.T) {
	body := environmentBody{
		Payload: EnvironmentPayload{
			Checks: []CheckPayload{
				{
					Name:     "check 1",
					Current:  "1",
					Required: "2",
				},
				{
					Name:     "check 2",
					Current:  "something",
					Required: "something better",
				},
			},
			Tuners: []TunerPayload{
				{
					Name:      "tuner 1",
					Enabled:   true,
					Supported: false,
				},
				{
					Name:      "tuner 2",
					ErrorMsg:  "tuner 2 failed",
					Enabled:   true,
					Supported: true,
				},
			},
			ErrorMsg: "tuner 2 failed",
		},
		NodeUUID:     "awe-1231-sdfasd-13-saddasdf-as123sdf",
		NodeID:       1,
		Organization: "test.vectorized.io",
		CPUCores:     12,
		CPUModel:     "AMD Ryzen 9 3900X 12-Core Processor",
		CloudVendor:  "AWS",
		RPVersion:    "release-0.99.8 (rev a2b48491)",
		VMType:       "i3.4xlarge",
		OSInfo:       "x86_64 5.8.9-200.fc32.x86_64 #1 SMP Mon Sep 14 18:28:45 UTC 2020 \"Fedora release 32 (Thirty Two)\"",
		SentAt:       time.Now(),
	}
	bs, err := json.Marshal(body)
	require.NoError(t, err)

	// Deep-copy the body that will be sent.
	expected := environmentBody{}
	err = json.Unmarshal(bs, &expected)
	require.NoError(t, err)

	env := "only-testing-nbd"
	t.Setenv("REDPANDA_ENVIRONMENT", env)
	defer func() {
		t.Setenv("REDPANDA_ENVIRONMENT", "")
	}()

	expected.Environment = env

	expectedBytes, err := json.Marshal(expected)
	require.NoError(t, err)

	ts := httptest.NewServer(
		http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
			b, err := io.ReadAll(r.Body)
			require.NoError(t, err)
			require.Exactly(t, expectedBytes, b)
			w.WriteHeader(http.StatusOK)
		}),
	)
	defer ts.Close()

	conf := config.Default()
	conf.Rpk.EnableUsageStats = true
	err = sendEnvironmentToURL(body, ts.URL, *conf)
	require.NoError(t, err)
}

func TestSkipSendEnvironment(t *testing.T) {
	ts := httptest.NewServer(
		http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
			require.FailNow(
				t,
				"The request shouldn't have been sent if metrics collection is disabled",
			)
		}),
	)
	defer ts.Close()

	conf := config.Default()
	conf.Rpk.EnableUsageStats = false
	err := sendEnvironmentToURL(environmentBody{}, ts.URL, *conf)
	require.NoError(t, err)
}
