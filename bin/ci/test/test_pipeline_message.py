import logging
from unittest.mock import AsyncMock, patch

import pytest

from pipeline_message import (
    get_failed_test_summary_message,
    get_problem_jobs,
    get_trace_failures,
    main,
    process_problem_jobs,
    sort_failed_tests_by_status,
    unexpected_improvements,
)


def test_get_problem_jobs():
    jobs = [
        {"stage": "build", "status": "failed"},
        {"stage": "test", "status": "canceled"},
        {"stage": "postmerge", "status": "failed"},
        {"stage": "performance", "status": "failed"},
        {"stage": "deploy", "status": "failed"},
    ]

    problem_jobs = get_problem_jobs(jobs)

    assert len(problem_jobs) == 3
    assert problem_jobs[0]["stage"] == "build"
    assert problem_jobs[1]["stage"] == "test"
    assert problem_jobs[2]["stage"] == "deploy"


def test_sort_failed_tests_by_status():
    failures_csv = """\
Test1,UnexpectedImprovement
Test2,Fail
Test3,Crash
Test4,Timeout
Test5,Fail
Test6,UnexpectedImprovement
"""
    sorted_tests = sort_failed_tests_by_status(failures_csv)

    assert len(sorted_tests["unexpected_improvements"]) == 2
    assert len(sorted_tests["fails"]) == 2
    assert len(sorted_tests["crashes"]) == 1
    assert len(sorted_tests["timeouts"]) == 1

    assert sorted_tests["unexpected_improvements"] == [
        "Test1,UnexpectedImprovement",
        "Test6,UnexpectedImprovement",
    ]
    assert sorted_tests["fails"] == ["Test2,Fail", "Test5,Fail"]
    assert sorted_tests["crashes"] == ["Test3,Crash"]
    assert sorted_tests["timeouts"] == ["Test4,Timeout"]


def test_get_failed_test_summary_message():
    failed_test_array = {
        "unexpected_improvements": [
            "test1 UnexpectedImprovement",
            "test2 UnexpectedImprovement",
        ],
        "fails": ["test3 Fail", "test4 Fail", "test5 Fail"],
        "crashes": ["test6 Crash"],
        "timeouts": [],
    }

    summary_message = get_failed_test_summary_message(failed_test_array)

    assert "<summary>" in summary_message
    assert "2 improved tests" in summary_message
    assert "3 failed tests" in summary_message
    assert "1 crashed test" in summary_message
    assert "</summary>" in summary_message


def test_unexpected_improvements():
    message = "<summary>"
    failed_test_array = {
        "unexpected_improvements": ["test_improvement_1", "test_improvement_2"],
        "fails": [],
        "crashes": [],
        "timeouts": [],
    }
    result = unexpected_improvements(failed_test_array)
    assert result == " 2 improved tests", f"Unexpected result: {result}"


@pytest.mark.asyncio
@patch("pipeline_message.get_pipeline_status", new_callable=AsyncMock)
async def test_gitlab_api_failure(mock_get_pipeline_status):
    mock_get_pipeline_status.side_effect = Exception("GitLab API not responding")
    message = await main("1234567")
    assert message == ""


@pytest.mark.asyncio
async def test_no_message_when_pipeline_not_failed():
    project_id = "176"
    pipeline_id = "12345"

    with patch(
        "pipeline_message.get_pipeline_status", new_callable=AsyncMock
    ) as mock_get_pipeline_status:
        mock_get_pipeline_status.return_value = "success"

        message = await main(pipeline_id, project_id)
        assert (
            message == ""
        ), f"Expected no message for successful pipeline, but got: {message}"


@pytest.mark.asyncio
async def test_single_problem_job_not_summarized():
    session = AsyncMock()
    project_id = "176"
    problem_jobs = [
        {
            "id": 1234,
            "name": "test-job",
            "web_url": "http://example.com/job/1234",
            "status": "canceled",
        }
    ]

    mock_response = AsyncMock()
    mock_response.status = 200
    mock_response.text.return_value = ""  # Empty CSV response for test
    session.get.return_value = mock_response

    message = await process_problem_jobs(session, project_id, problem_jobs)

    assert "summary" not in message
    assert "[test-job](http://example.com/job/1234)" in message


@pytest.mark.asyncio
@patch("pipeline_message.get_project_json", new_callable=AsyncMock)
@patch("pipeline_message.aiohttp.ClientSession", autospec=True)
async def test_get_trace_failures_no_response(
    mock_client_session_cls, mock_get_project_json, caplog
):
    caplog.set_level(logging.DEBUG)
    namespace = "mesa"
    mock_get_project_json.return_value = {"path": namespace}

    mock_get = AsyncMock()
    mock_get.status = 404

    mock_session_instance = mock_client_session_cls.return_value
    mock_session_instance.get.return_value = mock_get

    job_id = 12345678
    job = {"id": job_id}
    url = await get_trace_failures(mock_session_instance, "176", job)

    assert url == ""

    expected_log_message = f"No response from: https://mesa.pages.freedesktop.org/-/{namespace}/-/jobs/{job_id}/artifacts/results/summary/problems.html"
    assert any(expected_log_message in record.message for record in caplog.records)
